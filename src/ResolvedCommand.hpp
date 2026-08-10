#pragma once

#include "Builtin.hpp"
#include "Common.hpp"
#include "Path.hpp"

namespace koshka {

class ResolvedCommand
{
public:
  enum class Kind : u8
  {
    Builtin,
    Program,
    Unresolved,
  };

  Kind kind{Kind::Program};
  Builtin::Kind builtin_kind{};
  Path program_path{};
  i32 unresolved_status{127};

  mustuse static ResolvedCommand from_builtin(Builtin::Kind chosen_builtin)
  {
    ResolvedCommand resolved{};
    resolved.kind = Kind::Builtin;
    resolved.builtin_kind = chosen_builtin;
    return resolved;
  }

  mustuse static ResolvedCommand from_program(Path path)
  {
    ResolvedCommand resolved{};
    resolved.kind = Kind::Program;
    resolved.program_path = steal(path);
    return resolved;
  }

  mustuse static ResolvedCommand from_unresolved(i32 resolution_status)
  {
    ResolvedCommand resolved{};
    resolved.kind = Kind::Unresolved;
    resolved.unresolved_status = resolution_status;
    return resolved;
  }

  mustuse bool is_builtin() const { return kind == Kind::Builtin; }
  mustuse bool is_unresolved() const { return kind == Kind::Unresolved; }
};

} // namespace koshka
