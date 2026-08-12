#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-HSacdflmnpstuvw] [limit]");

HELP_DESCRIPTION_DECL(
    "The ulimit builtin reads or sets a resource limit of the shell.");

FLAG(CPU_TIME, Bool, 't', "", "The most CPU time in seconds.");
FLAG(FILE_SIZE, Bool, 'f', "",
     "The largest file the shell may create, in blocks.");
FLAG(DATA_SIZE, Bool, 'd', "", "The data segment size in kbytes.");
FLAG(STACK_SIZE, Bool, 's', "", "The stack size in kbytes.");
FLAG(CORE_SIZE, Bool, 'c', "", "The core dump size in blocks.");
FLAG(RSS_SIZE, Bool, 'm', "", "The resident set size in kbytes.");
FLAG(LOCKED_MEMORY, Bool, 'l', "", "The locked-in-memory size in kbytes.");
FLAG(PROCESSES_P, Bool, 'p', "", "The pipe buffer size in 512-byte blocks.");
FLAG(OPEN_FILES, Bool, 'n', "", "The largest number of open file descriptors.");
FLAG(VIRTUAL_MEMORY, Bool, 'v', "", "The virtual memory size in kbytes.");
FLAG(FILE_LOCKS, Bool, 'w', "", "The most file locks.");
FLAG(PROCESSES, Bool, 'u', "", "The most processes for the user.");
FLAG(ALL, Bool, 'a', "", "Report every limit.");
FLAG(HARD, Bool, 'H', "", "Read or set the hard limit.");
FLAG(SOFT, Bool, 'S', "", "Read or set the soft limit.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Ulimit);

namespace koshka {

namespace {

/* The order matches dash so -a prints the same table. */
struct resource_entry
{
  const char *label;
  os::resource_kind kind;
  u64 units_per_value;
};

constexpr resource_entry RESOURCE_TABLE[] = {
    {"time(seconds)",         os::resource_kind::CpuSeconds,          1   },
    {"file(blocks)",          os::resource_kind::FileBlocks,          512 },
    {"data(kbytes)",          os::resource_kind::DataKbytes,          1024},
    {"stack(kbytes)",         os::resource_kind::StackKbytes,         1024},
    {"coredump(blocks)",      os::resource_kind::CoreBlocks,          512 },
    {"memory(kbytes)",        os::resource_kind::ResidentKbytes,      1024},
    {"locked memory(kbytes)", os::resource_kind::LockedMemoryKbytes,  1024},
    {"process",               os::resource_kind::Processes,           1   },
    {"nofiles",               os::resource_kind::OpenFiles,           1   },
    {"vmemory(kbytes)",       os::resource_kind::VirtualMemoryKbytes, 1024},
    {"locks",                 os::resource_kind::FileLocks,           1   },
    {"rtprio",                os::resource_kind::RealtimePriority,    1   },
};

fn block_factor(const resource_entry &entry, bool is_posix_mode) throws -> u64
{
  if (entry.kind == os::resource_kind::FileBlocks ||
      entry.kind == os::resource_kind::CoreBlocks)
  {
    return is_posix_mode ? 512 : 1024;
  }

  return entry.units_per_value;
}

fn selected_resource(bool &is_pipe_pseudo_out) throws -> resource_entry
{
  is_pipe_pseudo_out = false;
  if (FLAG_CPU_TIME.is_enabled())
    return {"time(seconds)", os::resource_kind::CpuSeconds, 1};
  if (FLAG_DATA_SIZE.is_enabled())
    return {"data(kbytes)", os::resource_kind::DataKbytes, 1024};
  if (FLAG_STACK_SIZE.is_enabled())
    return {"stack(kbytes)", os::resource_kind::StackKbytes, 1024};
  if (FLAG_CORE_SIZE.is_enabled())
    return {"coredump(blocks)", os::resource_kind::CoreBlocks, 512};
  if (FLAG_OPEN_FILES.is_enabled())
    return {"nofiles", os::resource_kind::OpenFiles, 1};
  if (FLAG_RSS_SIZE.is_enabled())
    return {"memory(kbytes)", os::resource_kind::ResidentKbytes, 1024};
  if (FLAG_LOCKED_MEMORY.is_enabled())
    return {"locked memory(kbytes)", os::resource_kind::LockedMemoryKbytes,
            1024};
  if (FLAG_PROCESSES_P.is_enabled()) {
    is_pipe_pseudo_out = true;
    return {"pipe size", os::resource_kind::OpenFiles, 512};
  }
  if (FLAG_PROCESSES.is_enabled())
    return {"process", os::resource_kind::Processes, 1};
  if (FLAG_VIRTUAL_MEMORY.is_enabled())
    return {"vmemory(kbytes)", os::resource_kind::VirtualMemoryKbytes, 1024};
  if (FLAG_FILE_LOCKS.is_enabled())
    return {"locks", os::resource_kind::FileLocks, 1};
  return {"file(blocks)", os::resource_kind::FileBlocks, 512};
}

fn render_limit(const os::resource_limit &limit, u64 divisor,
                Allocator allocator) throws -> String
{
  let const value = FLAG_HARD.is_enabled() ? limit.hard : limit.soft;
  if (value == os::RESOURCE_UNLIMITED) return String{allocator, "unlimited"};
  return String::from(value / divisor, allocator);
}

} // namespace

cold fn Ulimit::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  if (FLAG_ALL.is_enabled()) {
    let out = String{cxt.scratch_allocator()};
    for (let const &entry : RESOURCE_TABLE) {
      os::resource_limit limit{};
      if (!os::get_resource_limit(entry.kind, limit)) continue;
      let const label = String{cxt.scratch_allocator(), entry.label};
      out += label;
      for (usize pad = label.count(); pad < 20; pad++)
        out.push(' ');
      out.push(' ');
      out += render_limit(limit, block_factor(entry, cxt.is_posix_mode()),
                          cxt.scratch_allocator());
      out.push('\n');
    }
    ec.print_to_stdout(out);
    return 0;
  }

  bool is_pipe_pseudo = false;
  let const resource = selected_resource(is_pipe_pseudo);

  if (is_pipe_pseudo) {
    if (args.count() < 2) {
      ec.print_to_stdout("8\n");
      return 0;
    }

    throw ErrorWithDetails{"The pipe size cannot be modified",
                           "The pipe buffer is a fixed kernel limit, read it "
                           "with `ulimit -p`"};
  }

  os::resource_limit limit{};
  if (!os::get_resource_limit(resource.kind, limit))
    throw Error{"Unable to read the resource limit because " +
                os::last_system_error_message()};

  if (args.count() < 2) {
    LOG(Debug, "ulimit reading the '%s' limit", resource.label);
    ec.print_to_stdout(render_limit(limit,
                                    block_factor(resource, cxt.is_posix_mode()),
                                    cxt.scratch_allocator()) +
                       "\n");
    return 0;
  }

  LOG(Debug, "ulimit changing the '%s' limit to '%s'", resource.label,
      args[1].c_str());

  let const &requested = args[1];
  let const units = block_factor(resource, cxt.is_posix_mode());
  u64 value = os::RESOURCE_UNLIMITED;
  if (requested != "unlimited") {
    let const parsed =
        static_cast<u64>(std::strtoull(requested.c_str(), nullptr, 10));
    /* A scaled resource multiplies the operand by its unit, so an operand that
       would overflow the multiply saturates to unlimited the way bash reports
       it. */
    if (units != 0 && parsed > os::RESOURCE_UNLIMITED / units) {
      value = os::RESOURCE_UNLIMITED;
    } else {
      value = parsed * units;
    }
  }

  /* Naming neither -H nor -S, or both together, sets both, the way dash does.
   */
  if (FLAG_HARD.is_enabled() || !FLAG_SOFT.is_enabled()) {
    limit.hard = value;
  }
  if (FLAG_SOFT.is_enabled() || !FLAG_HARD.is_enabled()) {
    limit.soft = value;
  }

  if (!os::set_resource_limit(resource.kind, limit))
    throw Error{"Unable to set the resource limit because " +
                os::last_system_error_message()};

  return 0;
}

Ulimit::Ulimit() = default;

pure fn Ulimit::kind() const wontthrow -> Builtin::Kind { return Kind::Ulimit; }

} // namespace koshka
