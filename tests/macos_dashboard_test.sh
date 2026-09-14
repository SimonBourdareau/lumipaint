#!/bin/bash
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source "$repo_dir/install-macos.command"
events=()
notices=()
show_dialog() { events+=(prompt); return "$dialog_status"; }
show_notice() { notices+=("$1"); }
open_dashboard() { events+=(dashboard); return "$dashboard_status"; }
reveal_program() { events+=(finder); [[ $1 == "$program" ]]; return "$finder_status"; }
program="/example/User's Library/lumi_paint.littlefoot"
dialog_status=0
dashboard_status=1
finder_status=0
dashboard_handoff "$program"
[[ ${events[*]} == 'prompt dashboard finder' ]]
[[ ${#notices[@]} == 1 ]]
[[ ${notices[0]} == *'plugins are installed'* && ${notices[0]} == *'ROLI Connect'* ]]
[[ ${notices[0]} == *'do not need to upload it again'* ]]

events=(); notices=(); dashboard_status=0
dashboard_handoff "$program"
[[ ${events[*]} == 'prompt dashboard finder' && ${#notices[@]} == 0 ]]

events=(); notices=(); dialog_status=1
dashboard_handoff "$program"
[[ ${events[*]} == 'prompt' && ${#notices[@]} == 0 ]]

events=(); notices=(); dialog_status=0; finder_status=1
dashboard_handoff "$program"
[[ ${#notices[@]} == 1 && ${notices[0]} == *"$program"* ]]
[[ ${notices[0]} == *'plugins are installed'* ]]
echo 'PASS: unavailable Dashboard notice, successful handoff, cancellation and Finder failure.'
