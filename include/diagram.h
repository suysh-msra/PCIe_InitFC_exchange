#ifndef DIAGRAM_H
#define DIAGRAM_H

// ============================================================================
// Diagram Renderer
//
// Draws a two-column "lifeline" ASCII art diagram representing traffic
// between two PCIe devices.  Supports DLLP arrows, flit boxes, TLP boxes,
// credit gauges, and scenario banners.
//
// Output goes to both stdout (ANSI color) and a plain-text log file.
// ============================================================================

#include "pcie_types.h"

#include <fstream>
#include <string>
#include <vector>

namespace ansi {
    inline constexpr const char* RST = "\033[0m";
    inline constexpr const char* RED = "\033[1;91m";   // bright red
    inline constexpr const char* GRN = "\033[1;92m";   // bright green
    inline constexpr const char* YLW = "\033[1;93m";   // bright yellow
    inline constexpr const char* BLU = "\033[1;94m";   // bright blue
    inline constexpr const char* MAG = "\033[1;95m";   // bright magenta
    inline constexpr const char* CYN = "\033[1;96m";   // bright cyan
    inline constexpr const char* BLD = "\033[1;97m";   // bright white bold
    inline constexpr const char* DIM = "\033[0;37m";   // light gray (readable)
}

class Diagram {
public:
    static constexpr int LCOL = 10;
    static constexpr int RCOL = 78;
    static constexpr int IW   = RCOL - LCOL - 1;
    static constexpr int LW   = RCOL + 1;

    explicit Diagram(const std::string& log_filename);

    // ---- primitives ----
    void emit(const std::string& line, const char* color = "");
    std::string ll(const std::string& inner = "") const;
    void blank();

    // ---- structural elements ----
    void device_header(const std::string& left_name, const std::string& left_bdf,
                       const std::string& left_buf,
                       const std::string& right_name, const std::string& right_bdf,
                       const std::string& right_buf);
    void cycle_sep(int cycle);
    void phase_banner(const std::string& text);
    void scenario_banner(const std::string& title, const std::string& subtitle);
    void dl_up();

    // ---- DLLP arrows ----
    // show_vc: prefix each DLLP line with [VCx].
    // suppress_infinite: don't add "(infinite)" for 0/0 credits (used for merged mode).
    void dllp_group_right(const std::vector<DLLP>& dllps,
                          bool show_vc = false, bool suppress_infinite = false);
    void dllp_group_left(const std::vector<DLLP>& dllps,
                         bool show_vc = false, bool suppress_infinite = false);
    void dllp_single_right(const DLLP& d);
    void dllp_single_left(const DLLP& d);

    // ---- Flit boxes (PCIe 6.0+ flit mode) ----
    void flit_box_right(const Flit& f);
    void flit_box_left(const Flit& f);

    // ---- FC state transitions ----
    void state_left(FCInitState from, FCInitState to, const std::string& reason);
    void state_right(FCInitState from, FCInitState to, const std::string& reason);

    // ---- TLP boxes ----
    void tlp_box_right(const TLP& t, const CreditTracker& cred);
    void tlp_box_left(const TLP& t, const CreditTracker& cred);

    // ---- back-pressure ----
    void blocked_left(const TLP& t, const CreditTracker& cred);
    void blocked_right(const TLP& t, const CreditTracker& cred);

    // ---- annotations ----
    void note_left(const std::string& text);
    void note_right(const std::string& text);
    void note_center(const std::string& text);

    // ---- credit gauges ----
    // annotations: optional array of 3 strings appended after each gauge row
    //   (e.g., "(shared)", "(VC0)", etc.).  Pass nullptr for no annotations.
    void credit_gauge(const std::string& label, const CreditTracker creds[3],
                      const char* annotations[3] = nullptr);

private:
    std::ofstream file_;

    std::string              gauge_bar(const CreditTracker& c) const;
    std::vector<std::string> build_tlp_box_lines(const TLP& t) const;
    void render_tlp_box(const TLP& t, const CreditTracker& cred, bool left_to_right);
    void render_flit_box(const Flit& f, bool left_to_right);
    void blocked_impl(const TLP& t, const CreditTracker& cred, bool on_left);
};

#endif // DIAGRAM_H
