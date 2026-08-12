#pragma once

#define D(code, slug, summary, message, suggestion, related, tier, delivery)   \
  {slug,                                                                       \
   summary,                                                                    \
   message,                                                                    \
   suggestion,                                                                 \
   related,                                                                    \
   code,                                                                       \
   diagnostic_tier::tier,                                                      \
   diagnostic_delivery::delivery}
