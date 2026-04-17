#include "pcie_device.h"

#include <sstream>

// ============================================================================
//  Construction
// ============================================================================

PCIeDevice::PCIeDevice(const std::string& name, uint16_t bdf,
                       uint8_t ph, uint16_t pd, uint8_t nph, uint16_t npd,
                       uint8_t cplh, uint16_t cpld)
    : name_(name), bdf_(bdf), fc_mode_(FCMode::Vanilla), num_vcs_(1)
{
    set_vc_capacity(0, ph, pd, nph, npd, cplh, cpld);
}

PCIeDevice::PCIeDevice(const std::string& name, uint16_t bdf, FCMode mode, int num_vcs)
    : name_(name), bdf_(bdf), fc_mode_(mode), num_vcs_(num_vcs)
{}

void PCIeDevice::set_vc_capacity(int vc, uint8_t ph, uint16_t pd,
                                 uint8_t nph, uint16_t npd,
                                 uint8_t cplh, uint16_t cpld) {
    rx_hdr_cap_ [vc][(int)FCGroup::Posted]     = ph;
    rx_data_cap_[vc][(int)FCGroup::Posted]     = pd;
    rx_hdr_cap_ [vc][(int)FCGroup::NonPosted]  = nph;
    rx_data_cap_[vc][(int)FCGroup::NonPosted]  = npd;
    rx_hdr_cap_ [vc][(int)FCGroup::Completion] = cplh;
    rx_data_cap_[vc][(int)FCGroup::Completion] = cpld;
}

void PCIeDevice::set_merged(FCGroup g, bool merged) {
    merged_[(int)g] = merged;
}

// ============================================================================
//  Credit access
// ============================================================================

CreditTracker& PCIeDevice::get_tx_cred(FCGroup g, int vc) {
    if (merged_[(int)g]) return tx_cred_[0][(int)g];
    return tx_cred_[vc][(int)g];
}

const CreditTracker& PCIeDevice::get_tx_cred(FCGroup g, int vc) const {
    if (merged_[(int)g]) return tx_cred_[0][(int)g];
    return tx_cred_[vc][(int)g];
}

void PCIeDevice::get_tx_credit_view(int vc, CreditTracker out[NUM_FC_GROUPS]) const {
    for (int i = 0; i < NUM_FC_GROUPS; i++)
        out[i] = get_tx_cred(static_cast<FCGroup>(i), vc);
}

bool PCIeDevice::is_dl_up() const {
    return fc_state_ == FCInitState::FC_READY;
}

// ============================================================================
//  FC Init FSM — dispatches by mode
// ============================================================================

void PCIeDevice::fc_init_tick() {
    if (fc_mode_ == FCMode::FlitMode) {
        drain_rx_flits();
        switch (fc_state_) {
        case FCInitState::FC_INIT1:
            emit_initfc1_flit();
            if (initfc1_flit_received_)
                fc_state_ = FCInitState::FC_INIT2;
            break;
        case FCInitState::FC_INIT2:
            emit_initfc2_flit();
            if (initfc2_flit_received_)
                fc_state_ = FCInitState::FC_READY;
            break;
        case FCInitState::FC_READY:
            break;
        }
    } else {
        drain_rx_dllps();
        switch (fc_state_) {
        case FCInitState::FC_INIT1:
            emit_initfc1();
            if (all_initfc1_received())
                fc_state_ = FCInitState::FC_INIT2;
            break;
        case FCInitState::FC_INIT2:
            emit_initfc2();
            if (initfc2_received_)
                fc_state_ = FCInitState::FC_READY;
            break;
        case FCInitState::FC_READY:
            break;
        }
    }
}

bool PCIeDevice::all_initfc1_received() const {
    for (int vc = 0; vc < num_vcs_; vc++)
        for (int g = 0; g < NUM_FC_GROUPS; g++)
            if (!initfc1_received_[vc][g]) return false;
    return true;
}

// ============================================================================
//  DLLP-based FC init (Vanilla / MergedCredits)
// ============================================================================

void PCIeDevice::drain_rx_dllps() {
    while (!rx_dllp_q_.empty()) {
        DLLP dllp = rx_dllp_q_.front();
        rx_dllp_q_.pop();
        int gi = (int)dllp_to_fc_group(dllp.type);
        int vc = dllp.vc;
        switch (dllp.type) {
        case DLLPType::InitFC1_P: case DLLPType::InitFC1_NP: case DLLPType::InitFC1_Cpl:
            initfc1_received_[vc][gi] = true;
            if (fc_mode_ == FCMode::MergedCredits && merged_[gi] && vc > 0) {
                // Non-VC0 for a merged group: credits are 0 (shares VC0's pool).
                // Don't overwrite the shared tracker — it was set by VC0's InitFC.
            } else {
                int tvc = (fc_mode_ == FCMode::MergedCredits && merged_[gi]) ? 0 : vc;
                tx_cred_[tvc][gi].set_limit(dllp.hdr_credits, dllp.data_credits);
            }
            break;
        case DLLPType::InitFC2_P: case DLLPType::InitFC2_NP: case DLLPType::InitFC2_Cpl:
            initfc2_received_ = true;
            break;
        case DLLPType::UpdateFC_P: case DLLPType::UpdateFC_NP: case DLLPType::UpdateFC_Cpl: {
            int tvc = (fc_mode_ == FCMode::MergedCredits && merged_[gi]) ? 0 : vc;
            tx_cred_[tvc][gi].update_limit(dllp.hdr_credits, dllp.data_credits);
            break;
        }
        }
    }
}

void PCIeDevice::emit_initfc1() {
    static const DLLPType types[] = {
        DLLPType::InitFC1_P, DLLPType::InitFC1_NP, DLLPType::InitFC1_Cpl};
    for (int vc = 0; vc < num_vcs_; vc++) {
        for (int i = 0; i < NUM_FC_GROUPS; i++) {
            if (!initfc1_sent_[vc][i]) {
                tx_dllp_q_.push({types[i], (uint8_t)vc,
                                 rx_hdr_cap_[vc][i], rx_data_cap_[vc][i]});
                initfc1_sent_[vc][i] = true;
            }
        }
    }
}

void PCIeDevice::emit_initfc2() {
    static const DLLPType types[] = {
        DLLPType::InitFC2_P, DLLPType::InitFC2_NP, DLLPType::InitFC2_Cpl};
    for (int vc = 0; vc < num_vcs_; vc++) {
        for (int i = 0; i < NUM_FC_GROUPS; i++) {
            if (!initfc2_sent_[vc][i]) {
                tx_dllp_q_.push({types[i], (uint8_t)vc,
                                 rx_hdr_cap_[vc][i], rx_data_cap_[vc][i]});
                initfc2_sent_[vc][i] = true;
            }
        }
    }
}

// ============================================================================
//  Flit-based FC init (FlitMode)
//  One flit carries all 3 FC groups' credits — no separate DLLPs needed.
// ============================================================================

void PCIeDevice::drain_rx_flits() {
    while (!rx_flit_q_.empty()) {
        Flit f = rx_flit_q_.front();
        rx_flit_q_.pop();
        switch (f.type) {
        case FlitType::InitFC1:
            initfc1_flit_received_ = true;
            for (int i = 0; i < NUM_FC_GROUPS; i++)
                tx_cred_[0][i].set_limit(f.hdr_credits[i], f.data_credits[i]);
            break;
        case FlitType::InitFC2:
            initfc2_flit_received_ = true;
            break;
        default:
            break;
        }
    }
}

void PCIeDevice::emit_initfc1_flit() {
    if (!initfc1_flit_sent_) {
        Flit f;
        f.type    = FlitType::InitFC1;
        f.seq_num = next_seq_++;
        for (int i = 0; i < NUM_FC_GROUPS; i++) {
            f.hdr_credits[i]  = rx_hdr_cap_[0][i];
            f.data_credits[i] = rx_data_cap_[0][i];
        }
        tx_flit_q_.push(f);
        initfc1_flit_sent_ = true;
    }
}

void PCIeDevice::emit_initfc2_flit() {
    if (!initfc2_flit_sent_) {
        Flit f;
        f.type    = FlitType::InitFC2;
        f.seq_num = next_seq_++;
        for (int i = 0; i < NUM_FC_GROUPS; i++) {
            f.hdr_credits[i]  = rx_hdr_cap_[0][i];
            f.data_credits[i] = rx_data_cap_[0][i];
        }
        tx_flit_q_.push(f);
        initfc2_flit_sent_ = true;
    }
}

// ============================================================================
//  Packet extraction
// ============================================================================

std::vector<DLLP> PCIeDevice::take_tx_dllps() {
    std::vector<DLLP> out;
    while (!tx_dllp_q_.empty()) {
        out.push_back(tx_dllp_q_.front());
        tx_dllp_q_.pop();
    }
    return out;
}

std::vector<Flit> PCIeDevice::take_tx_flits() {
    std::vector<Flit> out;
    while (!tx_flit_q_.empty()) {
        out.push_back(tx_flit_q_.front());
        tx_flit_q_.pop();
    }
    return out;
}

void PCIeDevice::send_update_fc(FCGroup grp, uint8_t hdr_cr, uint16_t data_cr, uint8_t vc) {
    DLLPType dtype;
    switch (grp) {
        case FCGroup::Posted:     dtype = DLLPType::UpdateFC_P;   break;
        case FCGroup::NonPosted:  dtype = DLLPType::UpdateFC_NP;  break;
        case FCGroup::Completion: dtype = DLLPType::UpdateFC_Cpl; break;
    }
    tx_dllp_q_.push({dtype, vc, hdr_cr, data_cr});
}

// ============================================================================
//  TLP construction
// ============================================================================

void PCIeDevice::enqueue_mem_write(uint32_t addr, const std::vector<uint32_t>& payload) {
    TLP tlp{};
    tlp.category            = TLPCategory::MemWrite32;
    tlp.header.fmt          = FMT_3DW_DATA;
    tlp.header.type         = TLPTYPE_MEM;
    tlp.header.tc           = 0;
    tlp.header.length       = static_cast<uint16_t>(payload.size());
    tlp.header.requester_id = bdf_;
    tlp.header.tag          = next_tag_++;
    tlp.header.first_dw_be  = 0xF;
    tlp.header.last_dw_be   = (payload.size() > 1) ? 0xF : 0x0;
    tlp.header.address      = addr & 0xFFFFFFFC;
    tlp.data                = payload;
    tlp.seq_num             = next_seq_++;
    tx_tlp_q_.push(tlp);
}

void PCIeDevice::enqueue_mem_read(uint32_t addr, uint16_t length_dw) {
    TLP tlp{};
    tlp.category            = TLPCategory::MemRead32;
    tlp.header.fmt          = FMT_3DW_NO_DATA;
    tlp.header.type         = TLPTYPE_MEM;
    tlp.header.tc           = 0;
    tlp.header.length       = length_dw;
    tlp.header.requester_id = bdf_;
    tlp.header.tag          = next_tag_++;
    tlp.header.first_dw_be  = 0xF;
    tlp.header.last_dw_be   = (length_dw > 1) ? 0xF : 0x0;
    tlp.header.address      = addr & 0xFFFFFFFC;
    tlp.seq_num             = next_seq_++;
    tx_tlp_q_.push(tlp);
}

void PCIeDevice::enqueue_completion(uint16_t req_id, uint8_t tag, uint16_t byte_count,
                                    const std::vector<uint32_t>& payload, uint32_t lower_addr) {
    TLP tlp{};
    tlp.category            = payload.empty() ? TLPCategory::Cpl : TLPCategory::CplD;
    tlp.header.fmt          = payload.empty() ? FMT_3DW_NO_DATA : FMT_3DW_DATA;
    tlp.header.type         = TLPTYPE_CPL;
    tlp.header.tc           = 0;
    tlp.header.length       = static_cast<uint16_t>(payload.size());
    tlp.header.completer_id = bdf_;
    tlp.header.requester_id = req_id;
    tlp.header.tag          = tag;
    tlp.header.cpl_status   = 0;
    tlp.header.byte_count   = byte_count;
    tlp.header.lower_address = static_cast<uint8_t>(lower_addr & 0x7F);
    tlp.data                = payload;
    tlp.seq_num             = next_seq_++;
    tx_tlp_q_.push(tlp);
}

// ============================================================================
//  TLP transmit / receive
// ============================================================================

bool PCIeDevice::try_transmit_tlp(TLP& out, int vc) {
    if (tx_tlp_q_.empty()) return false;
    const TLP& front = tx_tlp_q_.front();
    FCGroup grp = front.get_fc_group();
    CreditTracker& ct = get_tx_cred(grp, vc);
    if (!ct.has_credits(1, front.data_credit_cost()))
        return false;
    ct.consume(1, front.data_credit_cost());
    out = front;
    tx_tlp_q_.pop();
    return true;
}

bool PCIeDevice::has_pending_tlp() const { return !tx_tlp_q_.empty(); }
const TLP& PCIeDevice::peek_pending_tlp() const { return tx_tlp_q_.front(); }
void PCIeDevice::receive_tlp(const TLP& tlp) { rx_tlp_q_.push(tlp); }

std::vector<PCIeDevice::RxAction> PCIeDevice::process_rx_tlps() {
    std::vector<RxAction> actions;
    while (!rx_tlp_q_.empty()) {
        TLP tlp = rx_tlp_q_.front();
        rx_tlp_q_.pop();
        switch (tlp.category) {
        case TLPCategory::MemRead32: {
            uint16_t bc = tlp.header.length * 4;
            std::vector<uint32_t> rdata(tlp.header.length);
            for (size_t i = 0; i < rdata.size(); i++)
                rdata[i] = 0xCAFE0000 | static_cast<uint32_t>(i);
            enqueue_completion(tlp.header.requester_id, tlp.header.tag,
                               bc, rdata, tlp.header.address & 0x7F);
            actions.push_back({"Gen CplD for Tag=" + std::to_string(tlp.header.tag)
                + " BC=" + std::to_string(bc), true});
            break;
        }
        case TLPCategory::MemWrite32: {
            std::ostringstream oss;
            oss << "Absorbed MWr: " << tlp.header.length << "DW @ 0x"
                << hex32(tlp.header.address);
            actions.push_back({oss.str(), false});
            break;
        }
        case TLPCategory::CplD:
        case TLPCategory::Cpl:
            actions.push_back({"Cpl consumed: Tag=" + std::to_string(tlp.header.tag)
                + " BC=" + std::to_string(tlp.header.byte_count), false});
            break;
        }
    }
    return actions;
}
