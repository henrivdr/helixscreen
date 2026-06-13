// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_resume.h"

namespace helix {

std::vector<TerminalMatcher> snapmaker_terminal_matchers() {
    // Hardware-verified on a physical Snapmaker U1 (#991): dirty-bed aborts with
    // exception {id:532, code:1, message:"detected dirty bed"} — Terminal. Match
    // by message substring OR by exception id so either signal alone suffices.
    // NOT gated on sdcard-inactive: runout (id 523) also deactivates the SD, so
    // sdcard state cannot discriminate terminal-vs-recoverable here.
    return {
        TerminalMatcher{/*message_substr=*/"dirty bed", /*exception_id=*/-1,
                        /*require_sdcard_inactive=*/false},
        TerminalMatcher{/*message_substr=*/"", /*exception_id=*/532,
                        /*require_sdcard_inactive=*/false},
    };
}

} // namespace helix
