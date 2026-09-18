/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the getconf utility. It resolves supported system and
 * pathname configuration names through the platform configuration interfaces.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-v specification] system-variable | path-variable path");

HELP_DESCRIPTION_DECL("The getconf utility writes configuration values.");

FLAG(GETCONF_SPECIFICATION, String, 'v', "specification",
     "Use this POSIX specification.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Getconf);

namespace koshka::koshkit {

inline constexpr static_string_entry<os::system_configuration_key>
    SYSTEM_CONFIGURATION_ENTRIES[] = {
        {SSK("AIO_LISTIO_MAX"),                    os::system_configuration_key::AioListIoMax       },
        {SSK("AIO_MAX"),                           os::system_configuration_key::AioMax             },
        {SSK("AIO_PRIO_DELTA_MAX"),
         os::system_configuration_key::AioPriorityDeltaMax                                          },
        {SSK("ARG_MAX"),                           os::system_configuration_key::ArgMax             },
        {SSK("ATEXIT_MAX"),                        os::system_configuration_key::AtExitMax          },
        {SSK("BC_BASE_MAX"),                       os::system_configuration_key::BcBaseMax          },
        {SSK("BC_DIM_MAX"),                        os::system_configuration_key::BcDimensionMax     },
        {SSK("BC_SCALE_MAX"),                      os::system_configuration_key::BcScaleMax         },
        {SSK("BC_STRING_MAX"),                     os::system_configuration_key::BcStringMax        },
        {SSK("CHILD_MAX"),                         os::system_configuration_key::ChildMax           },
        {SSK("CLK_TCK"),                           os::system_configuration_key::ClockTicks         },
        {SSK("COLL_WEIGHTS_MAX"),
         os::system_configuration_key::CollationWeightsMax                                          },
        {SSK("DELAYTIMER_MAX"),                    os::system_configuration_key::DelayTimerMax      },
        {SSK("EXPR_NEST_MAX"),                     os::system_configuration_key::ExpressionNestMax  },
        {SSK("GETGR_R_SIZE_MAX"),
         os::system_configuration_key::GroupBufferSizeMax                                           },
        {SSK("GETPW_R_SIZE_MAX"),
         os::system_configuration_key::PasswordBufferSizeMax                                        },
        {SSK("HOST_NAME_MAX"),                     os::system_configuration_key::HostNameMax        },
        {SSK("IOV_MAX"),                           os::system_configuration_key::IoVectorMax        },
        {SSK("LINE_MAX"),                          os::system_configuration_key::LineMax            },
        {SSK("LOGIN_NAME_MAX"),                    os::system_configuration_key::LoginNameMax       },
        {SSK("MQ_OPEN_MAX"),                       os::system_configuration_key::MessageQueueOpenMax},
        {SSK("MQ_PRIO_MAX"),
         os::system_configuration_key::MessageQueuePriorityMax                                      },
        {SSK("NGROUPS_MAX"),                       os::system_configuration_key::GroupsMax          },
        {SSK("NPROCESSORS_CONF"),
         os::system_configuration_key::ProcessorConfigured                                          },
        {SSK("NPROCESSORS_ONLN"),
         os::system_configuration_key::ProcessorOnline                                              },
        {SSK("OPEN_MAX"),                          os::system_configuration_key::OpenMax            },
        {SSK("PAGESIZE"),                          os::system_configuration_key::PageSize           },
        {SSK("PAGE_SIZE"),                         os::system_configuration_key::PageSize           },
        {SSK("PASS_MAX"),                          os::system_configuration_key::PasswordMax        },
        {SSK("PHYS_PAGES"),                        os::system_configuration_key::PhysicalPages      },
        {SSK("POSIX2_CHAR_TERM"),
         os::system_configuration_key::Posix2CharTerminal                                           },
        {SSK("POSIX2_C_BIND"),                     os::system_configuration_key::Posix2CBind        },
        {SSK("POSIX2_C_DEV"),                      os::system_configuration_key::Posix2CDev         },
        {SSK("POSIX2_FORT_RUN"),
         os::system_configuration_key::Posix2FortranRun                                             },
        {SSK("POSIX2_LOCALEDEF"),
         os::system_configuration_key::Posix2LocaleDefinition                                       },
        {SSK("POSIX2_SW_DEV"),
         os::system_configuration_key::Posix2SoftwareDevelopment                                    },
        {SSK("POSIX2_UPE"),
         os::system_configuration_key::Posix2UserPortabilityUtilities                               },
        {SSK("POSIX2_VERSION"),                    os::system_configuration_key::Posix2Version      },
        {SSK("PTHREAD_DESTRUCTOR_ITERATIONS"),
         os::system_configuration_key::ThreadDestructorIterations                                   },
        {SSK("PTHREAD_KEYS_MAX"),                  os::system_configuration_key::ThreadKeysMax      },
        {SSK("PTHREAD_STACK_MIN"),
         os::system_configuration_key::ThreadStackMin                                               },
        {SSK("PTHREAD_THREADS_MAX"),
         os::system_configuration_key::ThreadCountMax                                               },
        {SSK("RE_DUP_MAX"),                        os::system_configuration_key::RegexDupMax        },
        {SSK("RTSIG_MAX"),                         os::system_configuration_key::RealtimeSignalMax  },
        {SSK("SEM_NSEMS_MAX"),                     os::system_configuration_key::SemaphoreCountMax  },
        {SSK("SEM_VALUE_MAX"),                     os::system_configuration_key::SemaphoreValueMax  },
        {SSK("SIGQUEUE_MAX"),                      os::system_configuration_key::SignalQueueMax     },
        {SSK("STREAM_MAX"),                        os::system_configuration_key::StreamMax          },
        {SSK("SYMLOOP_MAX"),                       os::system_configuration_key::SymbolicLinkLoopMax},
        {SSK("TIMER_MAX"),                         os::system_configuration_key::TimerMax           },
        {SSK("TTY_NAME_MAX"),                      os::system_configuration_key::TtyNameMax         },
        {SSK("TZNAME_MAX"),                        os::system_configuration_key::TimeZoneNameMax    },
        {SSK("XOPEN_UNIX"),                        os::system_configuration_key::XOpenUnix          },
        {SSK("XOPEN_UUCP"),                        os::system_configuration_key::XOpenUucp          },
        {SSK("_POSIX2_CHAR_TERM"),
         os::system_configuration_key::Posix2CharTerminal                                           },
        {SSK("_POSIX2_C_BIND"),                    os::system_configuration_key::Posix2CBind        },
        {SSK("_POSIX2_C_DEV"),                     os::system_configuration_key::Posix2CDev         },
        {SSK("_POSIX2_FORT_RUN"),
         os::system_configuration_key::Posix2FortranRun                                             },
        {SSK("_POSIX2_LOCALEDEF"),
         os::system_configuration_key::Posix2LocaleDefinition                                       },
        {SSK("_POSIX2_SW_DEV"),
         os::system_configuration_key::Posix2SoftwareDevelopment                                    },
        {SSK("_POSIX2_UPE"),
         os::system_configuration_key::Posix2UserPortabilityUtilities                               },
        {SSK("_POSIX2_VERSION"),                   os::system_configuration_key::Posix2Version      },
        {SSK("_POSIX_ADVISORY_INFO"),
         os::system_configuration_key::AdvisoryInfo                                                 },
        {SSK("_POSIX_ASYNCHRONOUS_IO"),
         os::system_configuration_key::AsynchronousIo                                               },
        {SSK("_POSIX_BARRIERS"),                   os::system_configuration_key::Barriers           },
        {SSK("_POSIX_CLOCK_SELECTION"),
         os::system_configuration_key::ClockSelection                                               },
        {SSK("_POSIX_CPUTIME"),                    os::system_configuration_key::CpuTime            },
        {SSK("_POSIX_DEVICE_CONTROL"),
         os::system_configuration_key::DeviceControl                                                },
        {SSK("_POSIX_FSYNC"),                      os::system_configuration_key::FileSync           },
        {SSK("_POSIX_IPV6"),                       os::system_configuration_key::IpV6               },
        {SSK("_POSIX_JOB_CONTROL"),                os::system_configuration_key::JobControl         },
        {SSK("_POSIX_MAPPED_FILES"),               os::system_configuration_key::MappedFiles        },
        {SSK("_POSIX_MEMLOCK"),                    os::system_configuration_key::MemoryLock         },
        {SSK("_POSIX_MEMLOCK_RANGE"),
         os::system_configuration_key::MemoryLockRange                                              },
        {SSK("_POSIX_MEMORY_PROTECTION"),
         os::system_configuration_key::MemoryProtection                                             },
        {SSK("_POSIX_MESSAGE_PASSING"),
         os::system_configuration_key::MessagePassing                                               },
        {SSK("_POSIX_MONOTONIC_CLOCK"),
         os::system_configuration_key::MonotonicClock                                               },
        {SSK("_POSIX_PRIORITIZED_IO"),
         os::system_configuration_key::PrioritizedIo                                                },
        {SSK("_POSIX_PRIORITY_SCHEDULING"),
         os::system_configuration_key::PriorityScheduling                                           },
        {SSK("_POSIX_RAW_SOCKETS"),                os::system_configuration_key::RawSockets         },
        {SSK("_POSIX_READER_WRITER_LOCKS"),
         os::system_configuration_key::ReaderWriterLocks                                            },
        {SSK("_POSIX_REALTIME_SIGNALS"),
         os::system_configuration_key::RealtimeSignals                                              },
        {SSK("_POSIX_REGEXP"),
         os::system_configuration_key::RegularExpressions                                           },
        {SSK("_POSIX_SAVED_IDS"),                  os::system_configuration_key::SavedIds           },
        {SSK("_POSIX_SEMAPHORES"),                 os::system_configuration_key::Semaphores         },
        {SSK("_POSIX_SHARED_MEMORY_OBJECTS"),
         os::system_configuration_key::SharedMemoryObjects                                          },
        {SSK("_POSIX_SHELL"),                      os::system_configuration_key::Shell              },
        {SSK("_POSIX_SPAWN"),                      os::system_configuration_key::Spawn              },
        {SSK("_POSIX_SPIN_LOCKS"),                 os::system_configuration_key::SpinLocks          },
        {SSK("_POSIX_SPORADIC_SERVER"),
         os::system_configuration_key::SporadicServer                                               },
        {SSK("_POSIX_SYNCHRONIZED_IO"),
         os::system_configuration_key::SynchronizedIo                                               },
        {SSK("_POSIX_THREADS"),                    os::system_configuration_key::Threads            },
        {SSK("_POSIX_THREAD_ATTR_STACKADDR"),
         os::system_configuration_key::ThreadAttributeStackAddress                                  },
        {SSK("_POSIX_THREAD_ATTR_STACKSIZE"),
         os::system_configuration_key::ThreadAttributeStackSize                                     },
        {SSK("_POSIX_THREAD_CPUTIME"),
         os::system_configuration_key::ThreadCpuTime                                                },
        {SSK("_POSIX_THREAD_PRIORITY_SCHEDULING"),
         os::system_configuration_key::ThreadPriorityScheduling                                     },
        {SSK("_POSIX_THREAD_PRIO_INHERIT"),
         os::system_configuration_key::ThreadPriorityInherit                                        },
        {SSK("_POSIX_THREAD_PRIO_PROTECT"),
         os::system_configuration_key::ThreadPriorityProtect                                        },
        {SSK("_POSIX_THREAD_PROCESS_SHARED"),
         os::system_configuration_key::ThreadProcessShared                                          },
        {SSK("_POSIX_THREAD_ROBUST_PRIO_INHERIT"),
         os::system_configuration_key::ThreadRobustPriorityInherit                                  },
        {SSK("_POSIX_THREAD_ROBUST_PRIO_PROTECT"),
         os::system_configuration_key::ThreadRobustPriorityProtect                                  },
        {SSK("_POSIX_THREAD_SAFE_FUNCTIONS"),
         os::system_configuration_key::ThreadSafeFunctions                                          },
        {SSK("_POSIX_THREAD_SPORADIC_SERVER"),
         os::system_configuration_key::ThreadSporadicServer                                         },
        {SSK("_POSIX_TIMEOUTS"),                   os::system_configuration_key::Timeouts           },
        {SSK("_POSIX_TIMERS"),                     os::system_configuration_key::Timers             },
        {SSK("_POSIX_TYPED_MEMORY_OBJECTS"),
         os::system_configuration_key::TypedMemoryObjects                                           },
        {SSK("_POSIX_V7_ILP32_OFF32"),
         os::system_configuration_key::V7Ilp32Off32                                                 },
        {SSK("_POSIX_V7_ILP32_OFFBIG"),
         os::system_configuration_key::V7Ilp32OffBig                                                },
        {SSK("_POSIX_V7_LP64_OFF64"),
         os::system_configuration_key::V7Lp64Off64                                                  },
        {SSK("_POSIX_V7_LPBIG_OFFBIG"),
         os::system_configuration_key::V7LpBigOffBig                                                },
        {SSK("_POSIX_V8_ILP32_OFF32"),
         os::system_configuration_key::V8Ilp32Off32                                                 },
        {SSK("_POSIX_V8_ILP32_OFFBIG"),
         os::system_configuration_key::V8Ilp32OffBig                                                },
        {SSK("_POSIX_V8_LP64_OFF64"),
         os::system_configuration_key::V8Lp64Off64                                                  },
        {SSK("_POSIX_V8_LPBIG_OFFBIG"),
         os::system_configuration_key::V8LpBigOffBig                                                },
        {SSK("_POSIX_VERSION"),                    os::system_configuration_key::PosixVersion       },
        {SSK("_XOPEN_CRYPT"),                      os::system_configuration_key::XOpenCrypt         },
        {SSK("_XOPEN_ENH_I18N"),
         os::system_configuration_key::XOpenEnhancedInternationalization                            },
        {SSK("_XOPEN_REALTIME"),                   os::system_configuration_key::XOpenRealtime      },
        {SSK("_XOPEN_REALTIME_THREADS"),
         os::system_configuration_key::XOpenRealtimeThreads                                         },
        {SSK("_XOPEN_SHM"),                        os::system_configuration_key::XOpenSharedMemory  },
        {SSK("_XOPEN_UNIX"),                       os::system_configuration_key::XOpenUnix          },
        {SSK("_XOPEN_UUCP"),                       os::system_configuration_key::XOpenUucp          },
        {SSK("_XOPEN_VERSION"),                    os::system_configuration_key::XOpenVersion       },
};
inline constexpr StaticStringMap SYSTEM_CONFIGURATIONS{
    SYSTEM_CONFIGURATION_ENTRIES};

inline constexpr static_string_entry<os::string_configuration_key>
    STRING_CONFIGURATION_ENTRIES[] = {
        {SSK("PATH"),                           os::string_configuration_key::Path         },
        {SSK("POSIX_V7_ILP32_OFF32_CFLAGS"),
         os::string_configuration_key::V7Ilp32Off32CFlags                                  },
        {SSK("POSIX_V7_ILP32_OFF32_LDFLAGS"),
         os::string_configuration_key::V7Ilp32Off32LdFlags                                 },
        {SSK("POSIX_V7_ILP32_OFF32_LIBS"),
         os::string_configuration_key::V7Ilp32Off32Libs                                    },
        {SSK("POSIX_V7_ILP32_OFFBIG_CFLAGS"),
         os::string_configuration_key::V7Ilp32OffBigCFlags                                 },
        {SSK("POSIX_V7_ILP32_OFFBIG_LDFLAGS"),
         os::string_configuration_key::V7Ilp32OffBigLdFlags                                },
        {SSK("POSIX_V7_ILP32_OFFBIG_LIBS"),
         os::string_configuration_key::V7Ilp32OffBigLibs                                   },
        {SSK("POSIX_V7_LP64_OFF64_CFLAGS"),
         os::string_configuration_key::V7Lp64Off64CFlags                                   },
        {SSK("POSIX_V7_LP64_OFF64_LDFLAGS"),
         os::string_configuration_key::V7Lp64Off64LdFlags                                  },
        {SSK("POSIX_V7_LP64_OFF64_LIBS"),
         os::string_configuration_key::V7Lp64Off64Libs                                     },
        {SSK("POSIX_V7_LPBIG_OFFBIG_CFLAGS"),
         os::string_configuration_key::V7LpBigOffBigCFlags                                 },
        {SSK("POSIX_V7_LPBIG_OFFBIG_LDFLAGS"),
         os::string_configuration_key::V7LpBigOffBigLdFlags                                },
        {SSK("POSIX_V7_LPBIG_OFFBIG_LIBS"),
         os::string_configuration_key::V7LpBigOffBigLibs                                   },
        {SSK("POSIX_V7_THREADS_CFLAGS"),
         os::string_configuration_key::V7WidthRestrictedEnvironments                       },
        {SSK("POSIX_V8_ILP32_OFF32_CFLAGS"),
         os::string_configuration_key::V8Ilp32Off32CFlags                                  },
        {SSK("POSIX_V8_ILP32_OFF32_LDFLAGS"),
         os::string_configuration_key::V8Ilp32Off32LdFlags                                 },
        {SSK("POSIX_V8_ILP32_OFF32_LIBS"),
         os::string_configuration_key::V8Ilp32Off32Libs                                    },
        {SSK("POSIX_V8_ILP32_OFFBIG_CFLAGS"),
         os::string_configuration_key::V8Ilp32OffBigCFlags                                 },
        {SSK("POSIX_V8_ILP32_OFFBIG_LDFLAGS"),
         os::string_configuration_key::V8Ilp32OffBigLdFlags                                },
        {SSK("POSIX_V8_ILP32_OFFBIG_LIBS"),
         os::string_configuration_key::V8Ilp32OffBigLibs                                   },
        {SSK("POSIX_V8_LP64_OFF64_CFLAGS"),
         os::string_configuration_key::V8Lp64Off64CFlags                                   },
        {SSK("POSIX_V8_LP64_OFF64_LDFLAGS"),
         os::string_configuration_key::V8Lp64Off64LdFlags                                  },
        {SSK("POSIX_V8_LP64_OFF64_LIBS"),
         os::string_configuration_key::V8Lp64Off64Libs                                     },
        {SSK("POSIX_V8_LPBIG_OFFBIG_CFLAGS"),
         os::string_configuration_key::V8LpBigOffBigCFlags                                 },
        {SSK("POSIX_V8_LPBIG_OFFBIG_LDFLAGS"),
         os::string_configuration_key::V8LpBigOffBigLdFlags                                },
        {SSK("POSIX_V8_LPBIG_OFFBIG_LIBS"),
         os::string_configuration_key::V8LpBigOffBigLibs                                   },
        {SSK("POSIX_V8_THREADS_CFLAGS"),
         os::string_configuration_key::V8ThreadsCFlags                                     },
        {SSK("POSIX_V8_THREADS_LDFLAGS"),
         os::string_configuration_key::V8ThreadsLdFlags                                    },
        {SSK("POSIX_V8_WIDTH_RESTRICTED_ENVS"),
         os::string_configuration_key::V8WidthRestrictedEnvironments                       },
        {SSK("V7_ENV"),                         os::string_configuration_key::V7Environment},
        {SSK("V8_ENV"),                         os::string_configuration_key::V8Environment},
};
inline constexpr StaticStringMap STRING_CONFIGURATIONS{
    STRING_CONFIGURATION_ENTRIES};

inline constexpr static_string_entry<os::path_configuration_key>
    PATH_CONFIGURATION_ENTRIES[] = {
        {SSK("FILESIZEBITS"),                os::path_configuration_key::FileSizeBits    },
        {SSK("LINK_MAX"),                    os::path_configuration_key::LinkMax         },
        {SSK("MAX_CANON"),                   os::path_configuration_key::MaxCanonical    },
        {SSK("MAX_INPUT"),                   os::path_configuration_key::MaxInput        },
        {SSK("NAME_MAX"),                    os::path_configuration_key::NameMax         },
        {SSK("PATH_MAX"),                    os::path_configuration_key::PathMax         },
        {SSK("PIPE_BUF"),                    os::path_configuration_key::PipeBuffer      },
        {SSK("POSIX2_SYMLINKS"),             os::path_configuration_key::TwoSymbolicLinks},
        {SSK("POSIX_ALLOC_SIZE_MIN"),
         os::path_configuration_key::AllocationSizeMin                                   },
        {SSK("POSIX_REC_INCR_XFER_SIZE"),
         os::path_configuration_key::RecommendedIncrementTransferSize                    },
        {SSK("POSIX_REC_MAX_XFER_SIZE"),
         os::path_configuration_key::RecommendedMaxTransferSize                          },
        {SSK("POSIX_REC_MIN_XFER_SIZE"),
         os::path_configuration_key::RecommendedMinTransferSize                          },
        {SSK("POSIX_REC_XFER_ALIGN"),
         os::path_configuration_key::RecommendedTransferAlignment                        },
        {SSK("SYMLINK_MAX"),                 os::path_configuration_key::SymbolicLinkMax },
        {SSK("TEXTDOMAIN_MAX"),              os::path_configuration_key::TextDomainMax   },
        {SSK("_POSIX_ASYNC_IO"),             os::path_configuration_key::AsyncIo         },
        {SSK("_POSIX_CHOWN_RESTRICTED"),
         os::path_configuration_key::ChownRestricted                                     },
        {SSK("_POSIX_FALLOC"),               os::path_configuration_key::Fallocate       },
        {SSK("_POSIX_NO_TRUNC"),             os::path_configuration_key::NoTrunc         },
        {SSK("_POSIX_PRIO_IO"),              os::path_configuration_key::PriorityIo      },
        {SSK("_POSIX_SYNC_IO"),              os::path_configuration_key::SyncIo          },
        {SSK("_POSIX_TIMESTAMP_RESOLUTION"),
         os::path_configuration_key::TimestampResolution                                 },
        {SSK("_POSIX_VDISABLE"),             os::path_configuration_key::DisableCharacter},
};
inline constexpr StaticStringMap PATH_CONFIGURATIONS{
    PATH_CONFIGURATION_ENTRIES};

Getconf::Getconf() = default;

pure fn Getconf::kind() const wontthrow -> Utility::Kind
{
  return Kind::Getconf;
}

fn Getconf::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty() || operands.count() > 2)
    return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_GETCONF_SPECIFICATION.is_set() &&
      FLAG_GETCONF_SPECIFICATION.value() != "POSIX_V7_LP64_OFF64" &&
      FLAG_GETCONF_SPECIFICATION.value() != "POSIX_V7_ILP32_OFFBIG")
  {
    report_soft_koshkit_util_error(
        ec, cxt, FLAG_GETCONF_SPECIFICATION.value_location(), args[0].view(),
        "unsupported specification '" +
            String{FLAG_GETCONF_SPECIFICATION.value()} + "'");
    return 2;
  }

  if (let const string_key = STRING_CONFIGURATIONS.find(operands[0].view());
      string_key.has_value())
  {
    if (operands.count() != 1)
      return report_usage_error(ec, cxt, args[0].view());

    let const value =
        os::string_configuration(*string_key, cxt.scratch_allocator());
    if (!value.has_value()) {
      ec.print_to_stdout("undefined\n");
    } else {
      ec.print_to_stdout(value->view());
      ec.print_to_stdout("\n");
    }
    return 0;
  }

  Maybe<i64> value;
  if (let const system_key = SYSTEM_CONFIGURATIONS.find(operands[0].view());
      system_key.has_value())
  {
    if (operands.count() != 1)
      return report_usage_error(ec, cxt, args[0].view());
    value = os::system_configuration(*system_key);
  } else if (let const path_key = PATH_CONFIGURATIONS.find(operands[0].view());
             path_key.has_value())
  {
    if (operands.count() != 2)
      return report_usage_error(ec, cxt, args[0].view());
    value = os::path_configuration(operands[1].view(), *path_key);
  } else {
    report_soft_koshkit_util_error(ec, cxt, operand_locations[0],
                                   args[0].view(),
                                   "unknown variable '" + operands[0] + "'");
    return 1;
  }

  if (!value.has_value() || *value == -1)
    ec.print_to_stdout("undefined\n");
  else
    ec.print_to_stdout(String::from(*value, cxt.scratch_allocator()) + "\n");
  return 0;
}

} // namespace koshka::koshkit
