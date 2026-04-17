// ============================================================================
// PCIe FC Init — Three Flavors
//
// Scenario A: Vanilla (separate credits, single VC, PCIe Gen1-5)
// Scenario B: Merged Credits (P & Cpl shared across VC0+VC1)
// Scenario C: Flit Mode (PCIe 6.0+, all credits in one 256B flit)
//
// Each scenario runs the full FC Init handshake, then demonstrates one TLP
// to show credits in action.
// ============================================================================

#include "diagram.h"
#include "pcie_device.h"

// ============================================================================
//  Helper: transfer DLLPs between devices via the Diagram
// ============================================================================

static void transfer_dllps(PCIeDevice& left, PCIeDevice& right, Diagram& d,
                           bool show_vc = false, bool suppress_inf = false) {
    auto left_dllps  = left.take_tx_dllps();
    auto right_dllps = right.take_tx_dllps();

    if (!left_dllps.empty()) {
        d.dllp_group_right(left_dllps, show_vc, suppress_inf);
        for (auto& dl : left_dllps) right.rx_dllp_q_.push(dl);
    }
    if (!right_dllps.empty()) {
        if (!left_dllps.empty()) d.blank();
        d.dllp_group_left(right_dllps, show_vc, suppress_inf);
        for (auto& dl : right_dllps) left.rx_dllp_q_.push(dl);
    }
}

// ============================================================================
//  Helper: transfer DLLPs grouped by VC (for merged credits)
// ============================================================================

static void transfer_dllps_by_vc(PCIeDevice& left, PCIeDevice& right, Diagram& d,
                                 int num_vcs) {
    auto left_dllps  = left.take_tx_dllps();
    auto right_dllps = right.take_tx_dllps();

    // Group by VC and render each VC's DLLPs with a separate arrow
    for (int vc = 0; vc < num_vcs; vc++) {
        std::vector<DLLP> vc_left, vc_right;
        for (auto& dl : left_dllps)
            if (dl.vc == vc) vc_left.push_back(dl);
        for (auto& dl : right_dllps)
            if (dl.vc == vc) vc_right.push_back(dl);

        if (!vc_left.empty()) {
            d.dllp_group_right(vc_left, true, true);
            for (auto& dl : vc_left) right.rx_dllp_q_.push(dl);
        }
        if (!vc_right.empty()) {
            if (!vc_left.empty()) d.blank();
            d.dllp_group_left(vc_right, true, true);
            for (auto& dl : vc_right) left.rx_dllp_q_.push(dl);
        }
        if (!vc_left.empty() || !vc_right.empty()) d.blank();
    }
}

// ============================================================================
//  Helper: transfer Flits between devices via the Diagram
// ============================================================================

static void transfer_flits(PCIeDevice& left, PCIeDevice& right, Diagram& d) {
    auto left_flits  = left.take_tx_flits();
    auto right_flits = right.take_tx_flits();

    for (auto& f : left_flits) {
        d.flit_box_right(f);
        right.rx_flit_q_.push(f);
    }
    if (!left_flits.empty() && !right_flits.empty()) d.blank();
    for (auto& f : right_flits) {
        d.flit_box_left(f);
        left.rx_flit_q_.push(f);
    }
}

// ============================================================================
//  Helper: run one FC init tick on both devices, render state transitions
// ============================================================================

static void tick_and_render_states(PCIeDevice& left, PCIeDevice& right, Diagram& d) {
    FCInitState lp = left.fc_state_, rp = right.fc_state_;
    left.fc_init_tick();
    if (left.fc_state_ != lp)
        d.state_left(lp, left.fc_state_,
            left.fc_state_ == FCInitState::FC_INIT2 ? "all InitFC1 groups received"
                                                    : "InitFC2 received - DL_Up!");
    right.fc_init_tick();
    if (right.fc_state_ != rp)
        d.state_right(rp, right.fc_state_,
            right.fc_state_ == FCInitState::FC_INIT2 ? "all InitFC1 groups received"
                                                     : "InitFC2 received - DL_Up!");
}

// ============================================================================
//  SCENARIO A: VANILLA (Separate Credits, Single VC)
// ============================================================================

static void run_vanilla(Diagram& dia) {
    dia.scenario_banner("SCENARIO A:  VANILLA FC INIT",
                        "Separate credits per FC group, single VC  (PCIe Gen1-5)");

    PCIeDevice rc("RootComplex", 0x0100, 8, 32, 4, 16, 0, 0);
    PCIeDevice ep("Endpoint",    0x0200, 4,  8, 2,  4, 4, 8);

    dia.device_header("RootComplex", "01:00.0", "PH=8 NPH=4 CplH=INF",
                      "Endpoint",    "02:00.0", "PH=4 NPH=2 CplH=4");

    dia.phase_banner("InitFC Handshake (3 DLLPs per direction per round)");

    int cycle = 0;

    // Cycle 0: emit InitFC1
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_dllps(rc, ep, dia);
    dia.blank();

    // Cycle 1: receive InitFC1 -> FC_INIT2
    dia.cycle_sep(cycle++);
    dia.blank();
    tick_and_render_states(rc, ep, dia);
    transfer_dllps(rc, ep, dia);
    dia.blank();

    // Cycle 2: emit InitFC2
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_dllps(rc, ep, dia);
    dia.blank();

    // Cycle 3: receive InitFC2 -> FC_READY
    dia.cycle_sep(cycle++);
    dia.blank();
    tick_and_render_states(rc, ep, dia);
    dia.dl_up();

    // Credit state
    dia.note_center("Credit State After FC Init:");
    {
        CreditTracker rc_view[NUM_FC_GROUPS], ep_view[NUM_FC_GROUPS];
        rc.get_tx_credit_view(0, rc_view);
        ep.get_tx_credit_view(0, ep_view);
        dia.credit_gauge("RC TX Credits (limit = EP buffers)", rc_view);
        dia.blank();
        dia.credit_gauge("EP TX Credits (limit = RC buffers)", ep_view);
    }
    dia.blank();

    // Brief TLP demo: one MWr
    dia.phase_banner("Brief TLP Demo:  MWr32  RC --> EP");
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.enqueue_mem_write(0x0000'1000, {0xDEAD'BEEF, 0x1234'5678});
    {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.get_tx_cred(tlp.get_fc_group()));
            ep.receive_tlp(tlp);
            for (auto& a : ep.process_rx_tlps()) dia.note_right(a.description);
        }
    }
    dia.blank();
    {
        CreditTracker rc_view[NUM_FC_GROUPS];
        rc.get_tx_credit_view(0, rc_view);
        dia.credit_gauge("RC TX Credits (after 1 MWr)", rc_view);
    }
    dia.blank();
}

// ============================================================================
//  SCENARIO B: MERGED CREDITS (P & Cpl shared across VC0 + VC1)
// ============================================================================

static void run_merged(Diagram& dia) {
    dia.scenario_banner("SCENARIO B:  MERGED CREDITS",
                        "P & Cpl share a single pool across VC0+VC1;  NP is per-VC");

    // RC: merged P (total PH=8 PD=32), merged Cpl (infinite), NP per-VC
    PCIeDevice rc("RootComplex", 0x0100, FCMode::MergedCredits, 2);
    rc.set_merged(FCGroup::Posted, true);
    rc.set_merged(FCGroup::Completion, true);
    rc.set_vc_capacity(0, /*PH*/8, /*PD*/32, /*NPH*/4, /*NPD*/16, /*CplH*/0, /*CplD*/0);
    rc.set_vc_capacity(1, /*PH*/0, /*PD*/ 0, /*NPH*/2, /*NPD*/ 8, /*CplH*/0, /*CplD*/0);

    // EP: merged P (total PH=4 PD=8), merged Cpl (total CplH=4 CplD=8), NP per-VC
    PCIeDevice ep("Endpoint", 0x0200, FCMode::MergedCredits, 2);
    ep.set_merged(FCGroup::Posted, true);
    ep.set_merged(FCGroup::Completion, true);
    ep.set_vc_capacity(0, /*PH*/4, /*PD*/8, /*NPH*/2, /*NPD*/4, /*CplH*/4, /*CplD*/8);
    ep.set_vc_capacity(1, /*PH*/0, /*PD*/0, /*NPH*/1, /*NPD*/2, /*CplH*/0, /*CplD*/0);

    dia.device_header("RootComplex", "01:00.0", "P:merged Cpl:merged",
                      "Endpoint",    "02:00.0", "P:merged Cpl:merged");

    dia.phase_banner("InitFC Handshake (6 DLLPs/dir: 3 groups x 2 VCs per round)");
    dia.note_center("VC0 carries total shared pool for merged groups (P, Cpl).");
    dia.note_center("VC1 carries 0/0 for merged groups = 'I share VC0's pool'.");
    dia.note_center("NP remains independent per VC.");
    dia.blank();

    int cycle = 0;

    // Cycle 0: emit InitFC1 for VC0 + VC1
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_dllps_by_vc(rc, ep, dia, 2);

    // Annotations
    dia.note_left("VC0 P/Cpl: total shared pool sizes");
    dia.note_left("VC1 P=0/Cpl=0: merged (NOT infinite!)");
    dia.blank();

    // Cycle 1: receive all InitFC1 -> FC_INIT2
    dia.cycle_sep(cycle++);
    dia.blank();
    tick_and_render_states(rc, ep, dia);
    transfer_dllps_by_vc(rc, ep, dia, 2);

    // Cycle 2: emit InitFC2 for VC0 + VC1
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_dllps_by_vc(rc, ep, dia, 2);

    // Cycle 3: receive InitFC2 -> FC_READY
    dia.cycle_sep(cycle++);
    dia.blank();
    tick_and_render_states(rc, ep, dia);
    dia.dl_up();

    // Credit state — show VC0 and VC1 views side by side
    dia.note_center("Credit State After FC Init:");
    dia.note_center("(P and Cpl show SAME shared tracker for both VCs)");
    dia.blank();
    {
        CreditTracker vc0_view[NUM_FC_GROUPS], vc1_view[NUM_FC_GROUPS];
        rc.get_tx_credit_view(0, vc0_view);
        rc.get_tx_credit_view(1, vc1_view);
        static const char* ann_vc0[] = {"(shared)", "(VC0)", "(shared)"};
        static const char* ann_vc1[] = {"(shared)", "(VC1)", "(shared)"};
        dia.credit_gauge("RC TX Credits -- VC0 view", vc0_view, ann_vc0);
        dia.blank();
        dia.credit_gauge("RC TX Credits -- VC1 view", vc1_view, ann_vc1);
    }
    dia.blank();

    // Demo: MWr on VC0 consumes shared P credits visible to both VCs
    dia.phase_banner("Shared Credit Demo:  MWr32 on VC0 affects VC1 too");
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.enqueue_mem_write(0x0000'1000, {0xAAAA'BBBB, 0xCCCC'DDDD});
    {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp, /*vc=*/0)) {
            dia.tlp_box_right(tlp, rc.get_tx_cred(FCGroup::Posted, 0));
            ep.receive_tlp(tlp);
            for (auto& a : ep.process_rx_tlps()) dia.note_right(a.description);
        }
    }
    dia.blank();
    dia.note_center("After MWr on VC0 — shared P credits consumed for BOTH VCs:");
    {
        CreditTracker vc0_view[NUM_FC_GROUPS], vc1_view[NUM_FC_GROUPS];
        rc.get_tx_credit_view(0, vc0_view);
        rc.get_tx_credit_view(1, vc1_view);
        static const char* ann_vc0[] = {"(shared)", "(VC0)", "(shared)"};
        static const char* ann_vc1[] = {"(shared)", "(VC1)", "(shared)"};
        dia.credit_gauge("RC TX Credits -- VC0 view", vc0_view, ann_vc0);
        dia.blank();
        dia.credit_gauge("RC TX Credits -- VC1 view", vc1_view, ann_vc1);
    }
    dia.blank();
    dia.note_center("^ P row is IDENTICAL in both views (shared pool)");
    dia.note_center("  NP row differs (independent per VC)");
    dia.blank();
}

// ============================================================================
//  SCENARIO C: FLIT MODE (PCIe 6.0+)
// ============================================================================

static void run_flit(Diagram& dia) {
    dia.scenario_banner("SCENARIO C:  FLIT MODE FC INIT",
                        "PCIe 6.0+ -- 256B flits, no DLLPs, all credits in 1 flit");

    // Larger buffers typical of Gen6 devices
    PCIeDevice rc("RootComplex", 0x0100, FCMode::FlitMode);
    rc.set_vc_capacity(0, 16, 64, 8, 32, 0, 0);

    PCIeDevice ep("Endpoint", 0x0200, FCMode::FlitMode);
    ep.set_vc_capacity(0, 8, 16, 4, 8, 8, 16);

    dia.device_header("RootComplex", "01:00.0", "PH=16 NPH=8 CplH=INF",
                      "Endpoint",    "02:00.0", "PH=8  NPH=4 CplH=8");

    dia.phase_banner("Flit-Based FC Init (1 flit/dir carries ALL 3 groups)");
    dia.note_center("Key difference: NO DLLPs at all. FC info embedded in flit.");
    dia.note_center("One 256B flit replaces three separate ~8B DLLPs per round.");
    dia.blank();

    int cycle = 0;

    // Cycle 0: emit InitFC1 flits
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_flits(rc, ep, dia);
    dia.blank();

    // Cycle 1: receive InitFC1 -> FC_INIT2
    dia.cycle_sep(cycle++);
    dia.blank();
    tick_and_render_states(rc, ep, dia);
    dia.blank();

    // Cycle 2: emit InitFC2 flits
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_flits(rc, ep, dia);
    dia.blank();

    // Cycle 3: receive InitFC2 -> FC_READY
    dia.cycle_sep(cycle++);
    dia.blank();
    tick_and_render_states(rc, ep, dia);
    dia.dl_up();

    // Credit state
    dia.note_center("Credit State After Flit-Mode FC Init:");
    {
        CreditTracker rc_view[NUM_FC_GROUPS], ep_view[NUM_FC_GROUPS];
        rc.get_tx_credit_view(0, rc_view);
        ep.get_tx_credit_view(0, ep_view);
        dia.credit_gauge("RC TX Credits (limit = EP buffers)", rc_view);
        dia.blank();
        dia.credit_gauge("EP TX Credits (limit = RC buffers)", ep_view);
    }
    dia.blank();

    // Brief TLP demo
    dia.phase_banner("Brief TLP Demo:  MWr32  RC --> EP  (packed inside flit)");
    dia.note_center("In flit mode, TLPs are packed into 256B flit payloads.");
    dia.note_center("Credit tracking still works per FC group.");
    dia.blank();
    dia.cycle_sep(cycle++);
    dia.blank();
    rc.enqueue_mem_write(0x0000'5000, {0xF1F1'F1F1, 0xE2E2'E2E2, 0xD3D3'D3D3, 0xC4C4'C4C4});
    {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.get_tx_cred(tlp.get_fc_group()));
            ep.receive_tlp(tlp);
            for (auto& a : ep.process_rx_tlps()) dia.note_right(a.description);
        }
    }
    dia.blank();
    {
        CreditTracker rc_view[NUM_FC_GROUPS];
        rc.get_tx_credit_view(0, rc_view);
        dia.credit_gauge("RC TX Credits (after 1 MWr)", rc_view);
    }
    dia.blank();
}

// ============================================================================
//  COMPARISON SUMMARY
// ============================================================================

static void run_summary(Diagram& dia) {
    dia.scenario_banner("COMPARISON SUMMARY",
                        "How the three FC init flavors differ");

    dia.note_center("+-------------------------------------------------------+");
    dia.note_center("|              |  Vanilla   | Merged Cr. | Flit Mode    |");
    dia.note_center("+-------------------------------------------------------+");
    dia.note_center("| Wire format  |  DLLPs     |  DLLPs     | 256B Flits   |");
    dia.note_center("| Pkts/round   |  3 (P/NP/C)|  6 (x2 VC) | 1 flit       |");
    dia.note_center("| Credit pools |  per-VC    |  shared P/C| per-VC       |");
    dia.note_center("| VCs shown    |  VC0 only  |  VC0 + VC1 | VC0 only     |");
    dia.note_center("| VC1 P=0 mean |  (n/a)     |  merged    | (n/a)        |");
    dia.note_center("| DLLPs exist? |  Yes       |  Yes       | No           |");
    dia.note_center("| PCIe Gen     |  1-5       |  1-5       | 6.0+         |");
    dia.note_center("+-------------------------------------------------------+");
    dia.blank();
    dia.note_center("Full log saved to: pcie_sim.log");
    dia.blank();
}

// ============================================================================
//  Main
// ============================================================================

int main() {
    Diagram dia("pcie_sim.log");

    run_vanilla(dia);
    run_merged(dia);
    run_flit(dia);
    run_summary(dia);

    return 0;
}
