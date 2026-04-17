#ifndef PCIE_DEVICE_H
#define PCIE_DEVICE_H

// ============================================================================
// PCIe Device Model
//
// Supports three FC initialization modes:
//   Vanilla       — single VC, separate credits (PCIe Gen1-5)
//   MergedCredits — 2 VCs, selected FC groups share a credit pool
//   FlitMode      — single VC, FC init via 256B flits (PCIe 6.0+)
//
// Silent — produces no output; all rendering is done externally.
// ============================================================================

#include "pcie_types.h"

#include <queue>
#include <string>
#include <vector>

class PCIeDevice {
public:
    // ---- identity / mode ----
    std::string name_;
    uint16_t    bdf_;
    FCMode      fc_mode_  = FCMode::Vanilla;
    int         num_vcs_  = 1;
    FCInitState fc_state_ = FCInitState::FC_INIT1;

    // Per-group merge flag: if true, that group shares a single pool across VCs.
    // Only meaningful in MergedCredits mode.
    bool merged_[NUM_FC_GROUPS] = {};

    // ---- RX buffer capacities (what we advertise via InitFC) ----
    // Indexed [vc][fc_group].  For Vanilla/FlitMode only [0] is used.
    uint8_t  rx_hdr_cap_ [MAX_VCS][NUM_FC_GROUPS] = {};
    uint16_t rx_data_cap_[MAX_VCS][NUM_FC_GROUPS] = {};

    // ---- TX credit trackers (limit set by partner's advertisement) ----
    // Indexed [vc][fc_group].  For merged groups, [0][g] is the shared tracker.
    CreditTracker tx_cred_[MAX_VCS][NUM_FC_GROUPS];

    // ---- FC Init handshake progress ----
    bool initfc1_sent_    [MAX_VCS][NUM_FC_GROUPS] = {};
    bool initfc1_received_[MAX_VCS][NUM_FC_GROUPS] = {};
    bool initfc2_sent_    [MAX_VCS][NUM_FC_GROUPS] = {};
    bool initfc2_received_ = false;

    // Flit-mode FC init tracking (all groups in one flit)
    bool initfc1_flit_sent_     = false;
    bool initfc1_flit_received_ = false;
    bool initfc2_flit_sent_     = false;
    bool initfc2_flit_received_ = false;

    // ---- Queues ----
    std::queue<DLLP> tx_dllp_q_;
    std::queue<DLLP> rx_dllp_q_;
    std::queue<Flit> tx_flit_q_;
    std::queue<Flit> rx_flit_q_;
    std::queue<TLP>  tx_tlp_q_;
    std::queue<TLP>  rx_tlp_q_;

    uint16_t next_seq_ = 0;
    uint8_t  next_tag_ = 0;

    // ---- Construction ----

    // Vanilla convenience: sets VC0 capacities inline
    PCIeDevice(const std::string& name, uint16_t bdf,
               uint8_t ph, uint16_t pd, uint8_t nph, uint16_t npd,
               uint8_t cplh, uint16_t cpld);

    // Mode-aware: call set_vc_capacity() afterwards
    PCIeDevice(const std::string& name, uint16_t bdf, FCMode mode, int num_vcs = 1);

    // ---- Configuration (call before fc_init_tick) ----
    void set_vc_capacity(int vc, uint8_t ph, uint16_t pd,
                         uint8_t nph, uint16_t npd,
                         uint8_t cplh, uint16_t cpld);
    void set_merged(FCGroup g, bool merged);

    // ---- Credit access ----

    // Returns the effective TX credit tracker for a given VC and group.
    // For merged groups, always returns the shared [0] tracker.
    CreditTracker&       get_tx_cred(FCGroup g, int vc = 0);
    const CreditTracker& get_tx_cred(FCGroup g, int vc = 0) const;

    // Fill out[3] with the effective credit view for a given VC.
    void get_tx_credit_view(int vc, CreditTracker out[NUM_FC_GROUPS]) const;

    // ---- FC Init FSM ----
    bool is_dl_up() const;
    void fc_init_tick();

    // ---- Packet extraction for rendering ----
    std::vector<DLLP> take_tx_dllps();
    std::vector<Flit> take_tx_flits();

    // ---- TLP construction ----
    void enqueue_mem_write(uint32_t addr, const std::vector<uint32_t>& payload);
    void enqueue_mem_read(uint32_t addr, uint16_t length_dw);
    void enqueue_completion(uint16_t req_id, uint8_t tag, uint16_t byte_count,
                            const std::vector<uint32_t>& payload, uint32_t lower_addr);

    // ---- TLP transmit / receive ----
    bool try_transmit_tlp(TLP& out, int vc = 0);

    bool       has_pending_tlp() const;
    const TLP& peek_pending_tlp() const;
    void       receive_tlp(const TLP& tlp);

    struct RxAction {
        std::string description;
        bool generated_completion;
    };
    std::vector<RxAction> process_rx_tlps();

    void send_update_fc(FCGroup grp, uint8_t hdr_cr, uint16_t data_cr, uint8_t vc = 0);

private:
    // DLLP-based FC init (Vanilla / MergedCredits)
    void drain_rx_dllps();
    void emit_initfc1();
    void emit_initfc2();

    // Flit-based FC init (FlitMode)
    void drain_rx_flits();
    void emit_initfc1_flit();
    void emit_initfc2_flit();

    bool all_initfc1_received() const;
};

#endif // PCIE_DEVICE_H
