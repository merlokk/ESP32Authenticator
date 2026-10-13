#include "web_policy.h"

namespace web {

const char *Name(Desired desired) {
    switch (desired) {
        case Desired::kOff: return "off";
        case Desired::kOn: return "on";
        case Desired::kAuto: return "auto";
    }
    return "off";
}

bool ShouldRun(Desired desired, bool network_wanted, bool stack_up, bool ap) {
    if (!network_wanted || !stack_up) {
        return false;
    }
    switch (desired) {
        case Desired::kOff: return false;
        case Desired::kOn: return true;
        case Desired::kAuto: return ap;
    }
    return false;
}

Reconcile Next(Desired desired, bool network_wanted, bool stack_up, bool ap, bool running) {
    const bool want = ShouldRun(desired, network_wanted, stack_up, ap);
    if (want == running) {
        return Reconcile::kNothing;
    }
    return want ? Reconcile::kStart : Reconcile::kStop;
}

}  // namespace web
