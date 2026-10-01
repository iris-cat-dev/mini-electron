
#ifndef net_CancelledReason_h
#define net_CancelledReason_h

namespace mini_electron {

enum CancelledReason {
    kNoCancelled,
    kNormalCancelled,
    kHookRedirectCancelled,
};

}

#endif // net_CancelledReason_h