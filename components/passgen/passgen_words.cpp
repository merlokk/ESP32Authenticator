#include "passgen_words.h"

#include "seed_phrase.h"

namespace passgen {

Words WordsOf(List list) {
    switch (list) {
        case List::kBip39: return {vault::seed::kWords, vault::seed::kWordListSize};
        case List::kSlip39: return {kSlip39, kSlip39Size};
        case List::kEff: break;
    }
    return {kEff, kEffSize};
}

}  // namespace passgen
