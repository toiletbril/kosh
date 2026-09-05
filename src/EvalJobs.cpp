/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements jobs evaluation. It applies the corresponding shell
 * semantics through EvalContext while preserving state, source locations,
 * and allocation ownership.
 */

#include "Common.hpp"
#include "Debug.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

fn EvalContext::set_last_background_pid(i64 pid) wontthrow -> void
{
  m_last_background_pid = pid;
}

fn EvalContext::register_job(os::process pid, StringView command,
                             i64 process_group_id) throws -> i32
{
  let new_job = job{};
  new_job.id = m_next_job_id++;
  new_job.pid = pid;
  new_job.process_id = os::process_id_of(pid);
  new_job.process_group_id = process_group_id;
  new_job.command = command;
  new_job.state = job::State::Running;
  m_jobs.push(steal(new_job));
  ASSERT(!m_jobs.is_empty());
  LOG(Info, "registered job %d", m_jobs.back().id);
  return m_jobs.back().id;
}

fn EvalContext::register_pipeline_job(const ArrayList<os::process> &processes,
                                      os::process primary_process,
                                      StringView command,
                                      i64 process_group_id) throws -> i32
{
  let new_job = job{};
  new_job.id = m_next_job_id++;
  new_job.pid = primary_process;
  new_job.process_id = os::process_id_of(primary_process);
  new_job.process_group_id = process_group_id;
  new_job.command = command;
  bool did_skip_primary = false;

  for (let const process : processes) {
    if (!did_skip_primary && process == primary_process) {
      did_skip_primary = true;
      continue;
    }
    new_job.earlier_pipeline_processes.push(process);
  }

  m_jobs.push(steal(new_job));
  ASSERT(!m_jobs.is_empty());
  LOG(Info, "registered pipeline job %d", m_jobs.back().id);
  return m_jobs.back().id;
}

fn EvalContext::register_stopped_job(os::process pid, StringView command,
                                     i32 status, i64 process_group_id) throws
    -> i32
{
  let const id = register_job(pid, command, process_group_id);
  job &registered = m_jobs.back();
  registered.state = job::State::Stopped;
  registered.stopped_status = status;
  return id;
}

fn EvalContext::notify_stopped_job(i32 id, StringView command) throws -> void
{
  print_error("[" + String::from(id, heap_allocator()) + "]+ Stopped  " +
              String{command} + "\n");
}

static fn poll_owned_processes(ArrayList<os::process> &processes) wontthrow
    -> Maybe<i32>
{
  let stopped_status = Maybe<i32>{None};
  for (usize process_position = processes.count(); process_position > 0;
       process_position--)
  {
    i32 status = 0;
    let const state = os::poll_process(processes[process_position - 1], status);
    if (state == os::process_state::Exited)
      processes.remove(process_position - 1);
    else if (state == os::process_state::Stopped)
      stopped_status = status;
  }

  return stopped_status;
}

fn EvalContext::update_jobs() throws -> void
{
  unused(poll_owned_processes(m_detached_job_processes));

  for (job &job : m_jobs) {
    if (job.state == job::State::Done) continue;

    let const earlier_stopped_status =
        poll_owned_processes(job.earlier_pipeline_processes);
    if (earlier_stopped_status.has_value()) {
      if (job.state != job::State::Stopped)
        job.has_unreported_state_change = true;
      job.state = job::State::Stopped;
      job.stopped_status = *earlier_stopped_status;
    }

    if (job.is_primary_process_active) {
      i32 status = 0;
      let const state = os::poll_process(job.pid, status);
      switch (state) {
      case os::process_state::Exited:
        job.is_primary_process_active = false;
        job.last_status = status;
        break;
      case os::process_state::Stopped:
        if (job.state != job::State::Stopped)
          job.has_unreported_state_change = true;
        job.state = job::State::Stopped;
        job.stopped_status = status;
        break;
      case os::process_state::Running:
        if (!earlier_stopped_status.has_value()) {
          if (job.state != job::State::Running)
            job.has_unreported_state_change = true;
          job.state = job::State::Running;
        }
        break;
      case os::process_state::Unchanged: break;
      }
    }

    if (!job.is_primary_process_active &&
        job.earlier_pipeline_processes.is_empty())
    {
      LOG(Info, "job %d finished with status %d", job.id, job.last_status);
      job.state = job::State::Done;
      job.has_unreported_state_change = true;
    }
  }
}

fn EvalContext::wait_for_job_processes(job &job, bool *was_stopped) throws
    -> i32
{
  if (job.state == job::State::Done) {
    if (was_stopped != nullptr) *was_stopped = false;
    return job.last_status;
  }
  if (job.state == job::State::Stopped) {
    if (was_stopped != nullptr) *was_stopped = true;
    return job.stopped_status;
  }

  if constexpr (os::HAS_CHILD_STATE_CHANGE_WAIT) {
    loop
    {
      update_jobs();
      if (job.state == job::State::Done) {
        if (was_stopped != nullptr) *was_stopped = false;
        return job.last_status;
      }
      if (job.state == job::State::Stopped) {
        if (was_stopped != nullptr) *was_stopped = true;
        return job.stopped_status;
      }
      os::wait_for_child_state_change();
    }
  }

  loop
  {
    let const fallback_process = job.is_primary_process_active
                                     ? job.pid
                                     : job.earlier_pipeline_processes.back();
    let process_was_stopped = false;
    let const process_status =
        os::wait_and_monitor_process(fallback_process, &process_was_stopped);

    if (process_was_stopped) {
      job.state = job::State::Stopped;
      job.stopped_status = process_status;
      if (was_stopped != nullptr) *was_stopped = true;
      return process_status;
    }

    if (fallback_process == job.pid) {
      job.last_status = process_status;
      job.is_primary_process_active = false;
    } else {
      for (usize process_position = job.earlier_pipeline_processes.count();
           process_position > 0; process_position--)
      {
        if (job.earlier_pipeline_processes[process_position - 1] !=
            fallback_process)
          continue;
        job.earlier_pipeline_processes.remove(process_position - 1);
        break;
      }
    }

    if (!job.is_primary_process_active &&
        job.earlier_pipeline_processes.is_empty())
    {
      job.state = job::State::Done;
      if (was_stopped != nullptr) *was_stopped = false;
      return job.last_status;
    }
  }
}

fn EvalContext::jobs() wontthrow -> ArrayList<job> & { return m_jobs; }

fn EvalContext::find_job(i32 id) wontthrow -> job *
{
  for (job &job : m_jobs)
    if (job.id == id) return &job;
  return nullptr;
}

fn EvalContext::find_job_index_by_spec(StringView spec) throws -> Maybe<usize>
{
  if (m_jobs.is_empty()) return koshka::None;

  StringView body = spec;
  if (!body.is_empty() && body[0] == '%') {
    body = body.substring(1);
  }

  if (body.is_empty() || body == "+" || body == "%") {
    return m_jobs.count() - 1;
  }
  if (body == "-")
    return m_jobs.count() >= 2 ? m_jobs.count() - 2 : m_jobs.count() - 1;

  if (let const parsed_value = body.to<i64>(); !parsed_value.is_error()) {
    for (usize i = 0; i < m_jobs.count(); i++)
      if (static_cast<i64>(m_jobs[i].id) == parsed_value.value()) return i;

    return koshka::None;
  }

  let const wants_substring_match = body[0] == '?';
  if (wants_substring_match) body = body.substring(1);

  if (body.is_empty()) return koshka::None;

  for (usize i = 0; i < m_jobs.count(); i++) {
    if (wants_substring_match) {
      if (m_jobs[i].command.find_substring(body).has_value()) return i;
    } else if (m_jobs[i].command.starts_with(body)) {
      return i;
    }
  }

  return koshka::None;
}

fn EvalContext::find_job_by_spec(StringView spec) throws -> job *
{
  if (let const index = find_job_index_by_spec(spec); index.has_value())
    return &m_jobs[*index];
  return nullptr;
}

fn EvalContext::most_recent_job() wontthrow -> job *
{
  /* Skip a finished job, so a bare fg or bg acts on a running or stopped job
     rather than a dead pid. */
  for (usize i = m_jobs.count(); i > 0; i--) {
    ASSERT(i - 1 < m_jobs.count());
    if (m_jobs[i - 1].state != job::State::Done) return &m_jobs[i - 1];
  }
  return nullptr;
}

fn EvalContext::forget_done_jobs() throws -> void
{
  let kept = ArrayList<job>{heap_allocator()};
  for (job &job : m_jobs) {
    if (job.state == job::State::Done) continue;
    kept.push(steal(job));
  }
  LOG(Debug, "dropping finished jobs, keeping %zu of %zu", kept.count(),
      m_jobs.count());
  m_jobs = steal(kept);
}

fn EvalContext::remove_job(i32 id) throws -> bool
{
  let removed_index = Maybe<usize>{None};
  for (usize position = 0; position < m_jobs.count(); position++)
    if (m_jobs[position].id == id) {
      removed_index = position;
      break;
    }
  if (!removed_index.has_value()) return false;

  let &removed = m_jobs[*removed_index];
  let const detached_count = removed.earlier_pipeline_processes.count() +
                             (removed.is_primary_process_active ? 1 : 0);
  let kept = ArrayList<job>{heap_allocator()};
  kept.reserve(m_jobs.count() - 1);
  m_detached_job_processes.reserve(m_detached_job_processes.count() +
                                   detached_count);

  if (removed.is_primary_process_active)
    m_detached_job_processes.push(removed.pid);
  for (let const process : removed.earlier_pipeline_processes)
    m_detached_job_processes.push(process);
  for (usize position = 0; position < m_jobs.count(); position++)
    if (position != *removed_index) kept.push(steal(m_jobs[position]));

  m_jobs = steal(kept);
  return true;
}

fn EvalContext::format_done_job_notifications(StringView line_ending) throws
    -> String
{
  update_jobs();

  let out = String{heap_allocator()};
  for (usize i = 0; i < m_jobs.count(); i++) {
    let const &job = m_jobs[i];
    if (job.state != job::State::Done) continue;

    char marker = ' ';
    if (i == m_jobs.count() - 1) {
      marker = '+';
    } else if (i == m_jobs.count() - 2) {
      marker = '-';
    }

    out += "[" + String::from(job.id, heap_allocator()) + "]";
    out.push(marker);
    out += " Done  ";
    out += job.command.c_str();
    out += line_ending;
  }

  forget_done_jobs();
  return out;
}

fn EvalContext::notify_done_jobs() throws -> void
{
  let const lines = format_done_job_notifications("\n");
  if (!lines.is_empty()) print_error(lines);
}

fn EvalContext::set_monitor(bool enabled) wontthrow -> void
{
  LOG(Info, "the monitor option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Monitor, enabled);
}

pure fn EvalContext::monitor() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Monitor);
}

fn EvalContext::set_notify(bool enabled) wontthrow -> void
{
  LOG(Info, "the notify option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Notify, enabled);
}

pure fn EvalContext::notify() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Notify);
}

} /* namespace koshka */
