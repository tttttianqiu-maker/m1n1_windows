#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m1n1-host-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-cc}

all_tests="
chainload_layout_test
hv_assisted_layout_test
display_guest_test
cpufreq_state_test
hv_autonomous_manifest_test
hv_bootstrap_manifest_test
hv_bootstrap_test
hv_autonomous_memory_test
hv_autonomous_profile_test
hv_autonomous_stage_test
hv_autonomous_boot_test
hv_autonomous_transport_test
hv_exception_lower_test
hv_diag_test
hv_fb_stream_test
hv_fb_stream_usb_limit_test
hv_fb_mirror_test
hv_guest_cpu_state_test
hv_irq_routes_test
hv_pci_state_test
hv_stage2_state_test
hv_stage_role_test
hv_launch_contract_test
hv_launch_j313_test
hv_launch_transport_test
hv_launch_snapshot_test
hv_launch_preflight_test
hv_launch_golden_j313_test
hv_nvme_queue_test
hv_nvme_fast_path_test
hv_fiq_fast_path_test
hv_tick_policy_test
hv_timer_delivery_test
hv_runtime_diag_debug_test
hv_runtime_diag_verbose_test
hv_runtime_diag_release_test
hv_watchdog_snapshot_test
hv_wfx_policy_test
hv_apple_input_contract_test
hv_sgi_pending_test
hv_sgi_diag_test
hv_vgic_diag_test
hv_vgic_redist_test
hv_xhci_handoff_test
iodev_console_backpressure_test
ringbuffer_test
uartproxy_event_test
usb_dwc3_bulk_state_test
"

if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2086
    set -- $all_tests
fi

for name in "$@"; do
    definitions=""
    sources="tests/$name.c"
    case "$name" in
        chainload_layout_test)
            sources="$sources src/chainload_layout.c"
            ;;
        hv_assisted_layout_test)
            sources="$sources src/hv_assisted_layout.c"
            ;;
        display_guest_test)
            definitions="-DDISPLAY_GUEST_HOST_TEST"
            sources="$sources src/display_guest.c"
            ;;
        cpufreq_state_test)
            sources="$sources src/cpufreq_state.c"
            ;;
        hv_autonomous_manifest_test)
            sources="$sources src/hv_autonomous_manifest.c src/hv_autonomous_profile.c"
            ;;
        hv_bootstrap_manifest_test)
            sources="$sources src/hv_bootstrap_manifest.c src/hv_autonomous_profile.c"
            ;;
        hv_bootstrap_test)
            definitions="-DHV_BOOTSTRAP_HOST_TEST"
            sources="$sources src/hv_bootstrap.c"
            ;;
        hv_autonomous_memory_test)
            sources="$sources src/hv_autonomous_memory.c"
            ;;
        hv_autonomous_profile_test)
            sources="$sources src/hv_autonomous_profile.c"
            ;;
        hv_autonomous_stage_test)
            sources="$sources src/hv_autonomous.c"
            ;;
        hv_autonomous_boot_test)
            sources="$sources src/hv_autonomous_boot.c"
            ;;
        hv_autonomous_transport_test)
            ;;
        hv_exception_lower_test)
            definitions="-DHV_EXCEPTION_LOWER_HOST_TEST"
            ;;
        hv_diag_test)
            definitions="-DHV_DIAG_HOST_TEST"
            sources="$sources src/hv_diag.c"
            ;;
        hv_fb_stream_test)
            definitions="-DHV_FB_STREAM_HOST_TEST -DHV_FB_STREAM_PAYLOAD_SIZE=4"
            sources="$sources src/hv_fb_stream.c"
            ;;
        hv_fb_mirror_test)
            definitions="-DHV_FB_MIRROR_HOST_TEST"
            sources="$sources src/hv_fb_mirror.c"
            ;;
        hv_fb_stream_usb_limit_test)
            definitions="-DHV_FB_STREAM_HOST_TEST -DUARTPROXY_EVENT_HOST_TEST"
            sources="$sources src/hv_fb_stream.c src/uartproxy_event.c"
            ;;
        hv_guest_cpu_state_test)
            sources="$sources src/hv_guest_cpu_state.c"
            ;;
        hv_irq_routes_test)
            definitions="-DHV_IRQ_ROUTES_HOST_TEST"
            sources="$sources src/hv_irq_routes.c"
            ;;
        hv_pci_state_test)
            sources="$sources src/hv_pci_state.c"
            ;;
        hv_stage2_state_test)
            sources="$sources src/hv_stage2_state.c"
            ;;
        hv_stage_role_test)
            sources="$sources src/hv_stage_role.c"
            ;;
        hv_launch_contract_test)
            sources="$sources src/hv_launch_contract.c"
            ;;
        hv_launch_j313_test)
            definitions="-DHV_LAUNCH_J313_HOST_TEST -DHV_IRQ_ROUTES_HOST_TEST"
            sources="$sources src/hv_launch_j313.c src/hv_launch_snapshot.c src/hv_launch_contract.c src/hv_irq_routes.c"
            ;;
        hv_launch_transport_test)
            sources="$sources src/hv_launch_transport.c src/hv_launch_contract.c"
            ;;
        hv_launch_snapshot_test)
            sources="$sources src/hv_launch_snapshot.c src/hv_launch_contract.c"
            ;;
        hv_launch_preflight_test)
            sources="$sources src/hv_launch_preflight.c src/hv_launch_contract.c"
            ;;
        hv_launch_golden_j313_test)
            sources="$sources src/hv_launch_golden_j313.c src/hv_launch_contract.c"
            ;;
        hv_nvme_queue_test)
            definitions="-DVNVME_HOST_TEST"
            sources="$sources src/hv_nvme_queue.c"
            ;;
        hv_nvme_fast_path_test)
            definitions="-DHV_NVME_FAST_PATH_HOST_TEST"
            sources="$sources src/hv_nvme_fast_path.c"
            ;;
        hv_fiq_fast_path_test)
            definitions="-DHV_FIQ_FAST_PATH_HOST_TEST"
            sources="$sources src/hv_fiq_fast_path.c"
            ;;
        hv_tick_policy_test)
            sources="$sources src/hv_tick_policy.c"
            ;;
        hv_timer_delivery_test)
            definitions="-DHV_TIMER_DELIVERY_HOST_TEST"
            sources="$sources src/hv_timer_delivery.c"
            ;;
        hv_runtime_diag_debug_test)
            definitions="-DHV_RUNTIME_DIAG_HOST_TEST"
            ;;
        hv_runtime_diag_verbose_test)
            definitions="-DHV_RUNTIME_DIAG_HOST_TEST -DHV_RUNTIME_DIAG_VERBOSE"
            ;;
        hv_runtime_diag_release_test)
            definitions="-DHV_RUNTIME_DIAG_HOST_TEST -DRELEASE"
            ;;
        hv_watchdog_snapshot_test)
            definitions="-DHV_WATCHDOG_SNAPSHOT_HOST_TEST"
            sources="$sources src/hv_watchdog_snapshot.c"
            ;;
        hv_wfx_policy_test)
            definitions="-DHV_WFX_POLICY_HOST_TEST"
            ;;
        hv_apple_input_contract_test)
            definitions="-DHV_APPLE_INPUT_HOST_TEST"
            sources="$sources src/hv_apple_input.c"
            ;;
        hv_sgi_pending_test)
            definitions="-DHV_SGI_PENDING_HOST_TEST"
            sources="$sources src/hv_sgi_pending.c"
            ;;
        hv_sgi_diag_test)
            definitions="-DHV_SGI_DIAG_HOST_TEST"
            sources="$sources src/hv_sgi_diag.c"
            ;;
        hv_vgic_diag_test)
            definitions="-DHV_VGIC_DIAG_HOST_TEST"
            sources="$sources src/hv_vgic_diag.c"
            ;;
        hv_vgic_redist_test)
            definitions="-DHV_VGIC_REDIST_HOST_TEST"
            sources="$sources src/hv_vgic_redist.c"
            ;;
        hv_xhci_handoff_test)
            definitions="-DHV_XHCI_HANDOFF_HOST_TEST"
            sources="$sources src/hv_xhci_handoff.c"
            ;;
        iodev_console_backpressure_test)
            ;;
        ringbuffer_test)
            definitions="-DRINGBUFFER_HOST_TEST"
            sources="$sources src/ringbuffer.c"
            ;;
        uartproxy_event_test)
            definitions="-DUARTPROXY_EVENT_HOST_TEST"
            sources="$sources src/uartproxy_event.c"
            ;;
        usb_dwc3_bulk_state_test)
            definitions="-DUSB_DWC3_BULK_STATE_HOST_TEST"
            sources="$sources src/usb_dwc3_bulk_state.c"
            ;;
        *)
            echo "unknown host test: $name" >&2
            exit 2
            ;;
    esac

    echo "  HOSTCC  $name"
    # The lists above are fixed repository paths, not user-provided shell input.
    # shellcheck disable=SC2086
    "$cc" -std=c11 -Wall -Wextra -Werror $definitions $sources -o "$build_dir/$name"
    "$build_dir/$name"
done
