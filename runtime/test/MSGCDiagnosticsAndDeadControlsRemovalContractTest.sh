#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

require_absent() {
    local pattern=$1
    if rg -n --glob '*.{cpp,h,py,sh}' "$pattern" \
        "$ROOT_DIR/src" "$ROOT_DIR/build/scripts" "$ROOT_DIR/test" \
        -g '!MSGCDiagnosticsAndDeadControlsRemovalContractTest.sh'; then
        echo "obsolete MSGC diagnostic/control remained: $pattern" >&2
        exit 1
    fi
}

require_absent 'cjMSSweepAssist'
require_absent '"cjMSAssistBaseRatioPercent"|"cjMSAssistBacklogStepBuffers"|"cjMSAssistCreditDecayRounds"|"cjMSScavengeRounds"'
require_absent 'gcAssist(ProductiveCalls|NoTaskCalls|ActiveUs)_|markAssist(Calls|NoTaskCalls|ActiveUs|AdmissionAccepted|AdmissionRejectedBacklog)_'
require_absent 'cycleStats_\.floatingLiveInflated'
require_absent 'cycleStats_\.(rootsCount|markMainActiveUs|markMainSlices|markHelperWU|markHelperActiveUs|markHelperSlices|activeRegionCount)'
require_absent 'LastResetClearedSlotCount'
require_absent 'kMSAssistBaseRatioPercentDefault'

echo "MSGC dead controls and diagnostics removal contract: PASS"
