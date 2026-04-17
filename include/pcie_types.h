#ifndef PCIE_TYPES_H
#define PCIE_TYPES_H

// ============================================================================
// PCIe Protocol Types
//
// All enumerations, packet structures, and credit tracking types used by
// the FC Init / TLP simulation.  Header-only — no .cpp companion needed.
// ============================================================================

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
//  FC Mode — selects the flavor of FC initialization
// ============================================================================

enum class FCMode {
    Vanilla,        // Standard separate credits, single VC (PCIe Gen1-5)
    MergedCredits,  // Selected FC groups share a credit pool across VCs
    FlitMode        // PCIe 6.0+ flit-based FC initialization (no DLLPs)
};

inline const char* fc_mode_str(FCMode m) {
    switch (m) {
        case FCMode::Vanilla:       return "Vanilla (Separate Credits)";
        case FCMode::MergedCredits: return "Merged Credits";
        case FCMode::FlitMode:      return "Flit Mode (PCIe 6.0+)";
    }
    return "?";
}

// ============================================================================
//  Constants
// ============================================================================

static constexpr int NUM_FC_GROUPS = 3;
static constexpr int MAX_VCS      = 2;   // VC0 + VC1 for merged credit demos

// ============================================================================
//  Flow Control groups — each must be initialized independently
// ============================================================================

enum class FCGroup : int { Posted = 0, NonPosted = 1, Completion = 2 };

inline const char* fc_group_str(FCGroup g) {
    switch (g) {
        case FCGroup::Posted:     return "Posted";
        case FCGroup::NonPosted:  return "NonPosted";
        case FCGroup::Completion: return "Completion";
    }
    return "?";
}

inline const char* fc_group_short(FCGroup g) {
    switch (g) {
        case FCGroup::Posted:     return "P";
        case FCGroup::NonPosted:  return "NP";
        case FCGroup::Completion: return "Cpl";
    }
    return "?";
}

// ============================================================================
//  FC Init FSM states (PCIe Base Spec Section 3.4)
// ============================================================================

enum class FCInitState { FC_INIT1, FC_INIT2, FC_READY };

inline const char* fc_state_str(FCInitState s) {
    switch (s) {
        case FCInitState::FC_INIT1: return "FC_INIT1";
        case FCInitState::FC_INIT2: return "FC_INIT2";
        case FCInitState::FC_READY: return "FC_READY";
    }
    return "?";
}

// ============================================================================
//  DLLP types relevant to FC initialization and credit updates
// ============================================================================

enum class DLLPType {
    InitFC1_P, InitFC1_NP, InitFC1_Cpl,
    InitFC2_P, InitFC2_NP, InitFC2_Cpl,
    UpdateFC_P, UpdateFC_NP, UpdateFC_Cpl
};

inline const char* dllp_type_str(DLLPType t) {
    switch (t) {
        case DLLPType::InitFC1_P:    return "InitFC1-P";
        case DLLPType::InitFC1_NP:   return "InitFC1-NP";
        case DLLPType::InitFC1_Cpl:  return "InitFC1-Cpl";
        case DLLPType::InitFC2_P:    return "InitFC2-P";
        case DLLPType::InitFC2_NP:   return "InitFC2-NP";
        case DLLPType::InitFC2_Cpl:  return "InitFC2-Cpl";
        case DLLPType::UpdateFC_P:   return "UpdateFC-P";
        case DLLPType::UpdateFC_NP:  return "UpdateFC-NP";
        case DLLPType::UpdateFC_Cpl: return "UpdateFC-Cpl";
    }
    return "?";
}

inline FCGroup dllp_to_fc_group(DLLPType t) {
    switch (t) {
        case DLLPType::InitFC1_P:   case DLLPType::InitFC2_P:   case DLLPType::UpdateFC_P:   return FCGroup::Posted;
        case DLLPType::InitFC1_NP:  case DLLPType::InitFC2_NP:  case DLLPType::UpdateFC_NP:  return FCGroup::NonPosted;
        case DLLPType::InitFC1_Cpl: case DLLPType::InitFC2_Cpl: case DLLPType::UpdateFC_Cpl: return FCGroup::Completion;
    }
    return FCGroup::Posted;
}

// ============================================================================
//  DLLP packet — carries FC credit values, tagged with VC
// ============================================================================

struct DLLP {
    DLLPType type;
    uint8_t  vc = 0;
    uint8_t  hdr_credits  = 0;
    uint16_t data_credits = 0;
};

// ============================================================================
//  Flit types (PCIe 6.0+ Flit Mode)
//
//  In flit mode the entire Data Link Layer runs on fixed 256-byte flits.
//  DLLPs don't exist — ACK/NAK and FC updates are embedded in flit overhead.
//  An FC Init Flit carries all 3 FC groups' credits in a single 256B packet.
// ============================================================================

enum class FlitType {
    InitFC1,   // FC Init round 1 — advertise buffer capacity
    InitFC2,   // FC Init round 2 — confirmation
    NOP,       // Idle / keepalive flit
    TLP_Flit   // Flit carrying packed TLP data
};

inline const char* flit_type_str(FlitType t) {
    switch (t) {
        case FlitType::InitFC1:  return "FC-Init-1";
        case FlitType::InitFC2:  return "FC-Init-2";
        case FlitType::NOP:      return "NOP";
        case FlitType::TLP_Flit: return "TLP-Flit";
    }
    return "?";
}

// A 256-byte flit.  For FC Init flits, the payload carries credit info for
// all three FC groups simultaneously — far more efficient than 3 DLLPs.
struct Flit {
    FlitType type;
    uint16_t seq_num = 0;
    uint8_t  hdr_credits[NUM_FC_GROUPS]  = {};
    uint16_t data_credits[NUM_FC_GROUPS] = {};

    static constexpr int SIZE_BYTES    = 256;
    static constexpr int PAYLOAD_BYTES = 236;  // 256 - 8B DL hdr - 6B FEC - 6B CRC
};

// ============================================================================
//  TLP Fmt / Type / Category
// ============================================================================

enum TLPFmt : uint8_t {
    FMT_3DW_NO_DATA = 0b00,
    FMT_4DW_NO_DATA = 0b01,
    FMT_3DW_DATA    = 0b10,
    FMT_4DW_DATA    = 0b11
};

enum TLPTypeBits : uint8_t {
    TLPTYPE_MEM = 0b00000,
    TLPTYPE_CPL = 0b01010,
};

enum class TLPCategory { MemRead32, MemWrite32, CplD, Cpl };

inline const char* tlp_cat_str(TLPCategory c) {
    switch (c) {
        case TLPCategory::MemRead32:  return "MRd32";
        case TLPCategory::MemWrite32: return "MWr32";
        case TLPCategory::CplD:       return "CplD";
        case TLPCategory::Cpl:        return "Cpl";
    }
    return "?";
}

// ============================================================================
//  TLP Header & Packet
// ============================================================================

struct TLPHeader {
    uint8_t  fmt         = 0;
    uint8_t  type        = 0;
    uint8_t  tc          = 0;
    bool     td          = false;
    bool     ep          = false;
    uint8_t  attr        = 0;
    uint16_t length      = 0;
    uint16_t requester_id = 0;
    uint8_t  tag          = 0;
    uint8_t  last_dw_be   = 0;
    uint8_t  first_dw_be  = 0;
    uint32_t address      = 0;
    uint16_t completer_id  = 0;
    uint8_t  cpl_status    = 0;
    uint16_t byte_count    = 0;
    uint8_t  lower_address = 0;
};

struct TLP {
    TLPCategory           category = TLPCategory::MemRead32;
    TLPHeader             header;
    std::vector<uint32_t> data;
    uint16_t              seq_num = 0;

    FCGroup get_fc_group() const {
        switch (category) {
            case TLPCategory::MemWrite32: return FCGroup::Posted;
            case TLPCategory::MemRead32:  return FCGroup::NonPosted;
            case TLPCategory::CplD:
            case TLPCategory::Cpl:        return FCGroup::Completion;
        }
        return FCGroup::Posted;
    }
    bool has_data() const { return (header.fmt & 0b10) != 0; }

    uint16_t data_credit_cost() const {
        if (!has_data() || header.length == 0) return 0;
        return (header.length + 3) / 4;
    }
};

// ============================================================================
//  Credit Tracker (per FC group, on the TX side)
// ============================================================================

struct CreditTracker {
    uint8_t  hdr_limit     = 0;
    uint16_t data_limit    = 0;
    uint8_t  hdr_consumed  = 0;
    uint16_t data_consumed = 0;
    bool     infinite_hdr  = false;
    bool     infinite_data = false;

    void set_limit(uint8_t h, uint16_t d) {
        if (h == 0) infinite_hdr = true; else hdr_limit = h;
        if (d == 0) infinite_data = true; else data_limit = d;
    }

    bool has_credits(uint8_t hdr_need, uint16_t data_need) const {
        bool hdr_ok  = infinite_hdr  || (uint8_t)(hdr_limit - hdr_consumed) >= hdr_need;
        bool data_ok = infinite_data || (uint16_t)(data_limit - data_consumed) >= data_need;
        return hdr_ok && data_ok;
    }

    void consume(uint8_t h, uint16_t d) {
        if (!infinite_hdr)  hdr_consumed  += h;
        if (!infinite_data) data_consumed += d;
    }

    void update_limit(uint8_t h, uint16_t d) {
        if (!infinite_hdr)  hdr_limit  = h;
        if (!infinite_data) data_limit = d;
    }

    int hdr_avail() const {
        if (infinite_hdr) return -1;
        return (uint8_t)(hdr_limit - hdr_consumed);
    }
    int data_avail() const {
        if (infinite_data) return -1;
        return (uint16_t)(data_limit - data_consumed);
    }
};

// ============================================================================
//  Hex formatting helpers
// ============================================================================

inline std::string hex32(uint32_t v) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
    return ss.str();
}

inline std::string hex16(uint16_t v) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << v;
    return ss.str();
}

#endif // PCIE_TYPES_H
