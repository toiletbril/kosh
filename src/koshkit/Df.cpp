#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-kP] [file ...]");

HELP_DESCRIPTION_DECL("The df utility reports available filesystem space.");

FLAG(DF_KIBIBYTES, Bool, 'k', "kilobytes", "Use 1024-byte units.");
FLAG(DF_PORTABLE, Bool, 'P', "portability", "Use the POSIX output format.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Df);

namespace koshka::koshkit {

static fn scaled_blocks(u64 block_count, u64 block_size,
                        u64 output_unit) wontthrow -> u64
{
  let const byte_count = static_cast<u128>(block_count) * block_size;
  let const scaled_count = (byte_count + output_unit - 1) / output_unit;
  if (scaled_count > UINT64_MAX) return UINT64_MAX;

  return static_cast<u64>(scaled_count);
}

Df::Df() = default;

pure fn Df::kind() const wontthrow -> Utility::Kind { return Kind::Df; }

fn Df::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const output_unit = FLAG_DF_KIBIBYTES.is_enabled() ? 1024u : 512u;
  ec.print_to_stdout(
      FLAG_DF_KIBIBYTES.is_enabled()
          ? "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
          : "Filesystem 512-blocks Used Available Capacity Mounted on\n");
  i32 status = 0;

  let filesystems = ArrayList<os::mounted_filesystem>{cxt.scratch_allocator()};
  if (operands.is_empty()) {
    let mounted = os::mounted_filesystems();
    for (let &filesystem : mounted)
      filesystems.push(os::mounted_filesystem{steal(filesystem.source),
                                              steal(filesystem.target)});
  } else {
    for (let const &operand : operands)
      filesystems.push(
          os::mounted_filesystem{operand.clone(), operand.clone()});
  }

  for (const os::mounted_filesystem &mounted : filesystems) {
    os::filesystem_status filesystem{};
    if (!os::stat_filesystem(mounted.target.view(), filesystem)) {
      report_soft_koshkit_error(ec, cxt,
                                "df: cannot read '" + mounted.target +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let const total = scaled_blocks(filesystem.total_blocks,
                                    filesystem.block_size, output_unit);
    let const free = scaled_blocks(filesystem.free_blocks,
                                   filesystem.block_size, output_unit);
    let const available = scaled_blocks(filesystem.available_blocks,
                                        filesystem.block_size, output_unit);
    let const used = total > free ? total - free : 0;
    let const capacity_base = used + available;
    let const capacity = capacity_base == 0
                             ? 0
                             : static_cast<u64>((static_cast<u128>(used) * 100 +
                                                 capacity_base - 1) /
                                                capacity_base);
    ec.print_to_stdout(mounted.source + " " +
                       String::from(total, cxt.scratch_allocator()) + " " +
                       String::from(used, cxt.scratch_allocator()) + " " +
                       String::from(available, cxt.scratch_allocator()) + " " +
                       String::from(capacity, cxt.scratch_allocator()) + "% " +
                       mounted.target + "\n");
  }

  return status;
}

} // namespace koshka::koshkit
