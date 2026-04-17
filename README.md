# PCIe Flow Control Initialization & TLP Traffic Simulator

A C++17 simulation that models two PCIe devices performing the Data Link Layer
Flow Control (FC) handshake and exchanging Transaction Layer Packets (TLPs),
with full ASCII art visualization of the packet flow.

---

## PCIe Background

### The Problem Flow Control Solves

PCIe is a point-to-point serial interconnect where a transmitter can send
packets faster than a receiver can consume them. Without flow control, the
receiver's buffers would overflow and data would be lost. PCIe solves this
with a **credit-based flow control** scheme defined at the Data Link Layer.

### Credit-Based Flow Control

Each receiver advertises how much buffer space it has, expressed as
**credits** — one header credit per TLP header slot, and data credits in
units of 4 DW (16 bytes). The sender tracks a credit *limit* (set by the
receiver) and a credit *consumed* counter (incremented with each TLP sent).
A TLP can only be transmitted when sufficient credits are available:

```
Credits Available = Credit Limit - Credits Consumed   (modular arithmetic)
```

When the receiver frees buffer space, it sends an **UpdateFC DLLP** carrying
the new credit limit (an absolute high-water mark, not a delta).

An advertised value of **0 = infinite credits** — the sender is never blocked.
This is common for Completion credits on Root Complexes, which can always
sink completions.

### FC Groups

PCIe defines three independent Flow Control groups, each with its own header
and data credit pools:

| FC Group      | TLP Types                        | Why Separate?                         |
|---------------|----------------------------------|---------------------------------------|
| **Posted**    | Memory Writes (MWr)              | Fire-and-forget; no completion        |
| **Non-Posted**| Memory Reads (MRd), Config R/W   | Requires a completion; could deadlock |
| **Completion**| CplD, Cpl                        | Must always be sinkable to avoid deadlock |

Keeping them independent prevents a posted write flood from starving
completions — a classic deadlock scenario the PCIe spec explicitly guards
against (see PCIe Base Spec Section 2.6).

### The FC Initialization Handshake

Before any TLPs can flow, both ends of a PCIe link must exchange their
buffer capacities using **Data Link Layer Packets (DLLPs)**. The handshake
is a three-state FSM:

```
                   All 3 InitFC1              Any InitFC2
                   groups received             received
  +-----------+   ─────────────────>  +-----------+  ──────────────>  +-----------+
  | FC_INIT1  |     (emit InitFC2)    | FC_INIT2  |    (DL_Up!)      | FC_READY  |
  |           |                       |           |                   |           |
  | emit      |                       | emit      |                   | TLP       |
  | InitFC1   |                       | InitFC2   |                   | traffic   |
  +-----------+                       +-----------+                   +-----------+
```

1. **FC_INIT1**: Each device sends `InitFC1` DLLPs for all three FC groups
   (Posted, Non-Posted, Completion), advertising its receive buffer capacity.

2. **FC_INIT2**: After receiving all three `InitFC1` groups from the partner,
   the device transitions and sends `InitFC2` DLLPs (confirming the exchange).

3. **FC_READY**: After receiving at least one `InitFC2`, the link is
   considered **DL_Up** and TLP traffic can begin.

### TLP Header Format

Every TLP begins with a 3 or 4 DW header. The key fields in DW0:

```
 31  29 28  24 23          16 15              8 7               0
+------+------+--------------+----+---+---+----+----------------+
| Fmt  | Type |   Reserved   | TC | ? | ? |Attr|    Length       |
+------+------+--------------+----+---+---+----+----------------+
```

- **Fmt[1:0]**: `00`=3DW no data, `01`=4DW no data, `10`=3DW+data, `11`=4DW+data
- **Type[4:0]**: `00000`=Memory, `01010`=Completion, etc.
- **Length**: Payload size in DW (0 = 1024 DW)

DW1-DW2 vary by type (Requester ID / Tag / Address for requests;
Completer ID / Status / Byte Count for completions).

---

## What This Simulator Does

The simulator creates two devices — a **Root Complex** and an **Endpoint** —
connected by a modeled PCIe link, then runs five phases:

| Phase | Description |
|-------|-------------|
| **1** | **FC Init Handshake** — Both devices exchange InitFC1/InitFC2 DLLPs and transition through FC_INIT1 → FC_INIT2 → FC_READY. |
| **2** | **Posted Writes** — Root Complex sends MWr32 TLPs to the Endpoint, consuming Posted credits. |
| **3** | **Non-Posted Read + Completion** — Root Complex sends an MRd32; Endpoint responds with a CplD. Demonstrates the request/completion round-trip. |
| **4** | **Credit Exhaustion** — Root Complex exhausts all Posted header credits. Further writes are **blocked** (back-pressure). |
| **5** | **Credit Return** — Endpoint sends an UpdateFC DLLP to restore credits. Previously-blocked writes are retried and succeed. |

All output is rendered as a **two-column lifeline diagram** showing:
- DLLP arrows with credit values
- TLP header boxes with spec-compliant field encodings (Fmt, Type, TC, Length, ReqID, Tag, Address, etc.)
- FC state transitions
- Credit gauge bars showing utilization per FC group
- Back-pressure (BLOCKED) indicators with "have vs. need" credit details

Output goes to both **stdout** (with ANSI color) and a plain-text **`pcie_sim.log`** file.

---

## Project Structure

```
PCIe_parallel_handshakes/
├── README.md
├── Makefile
├── .gitignore
├── src/                        # Implementation files
│   ├── main.cpp                #   Simulation driver
│   ├── pcie_device.cpp         #   Device model implementation
│   └── diagram.cpp             #   ASCII art renderer implementation
├── include/                    # Public headers
│   ├── pcie_types.h            #   Protocol vocabulary (enums, structs)
│   ├── pcie_device.h           #   Device model interface
│   └── diagram.h               #   Renderer interface
├── standalone/                 # Monolithic single-file version
│   └── pcie_fc_sim.cpp         #   All-in-one (no multi-file build needed)
├── build/                      # Compiler output (generated)
└── output/                     # Simulation logs (generated)
```

Read the source files in this order to follow the dependency chain:

1. **`include/pcie_types.h`** — All protocol-level types with no external
   dependencies beyond the C++ standard library. Defines `FCGroup`,
   `FCInitState`, `DLLPType`, `TLPCategory`, `TLPHeader`, `TLP`,
   `CreditTracker`, and helper functions for converting enums to strings
   and formatting hex values.

2. **`include/pcie_device.h`** + **`src/pcie_device.cpp`** — The `PCIeDevice`
   class models one end of a PCIe link. It owns:
   - RX buffer capacities (what it advertises via InitFC)
   - TX credit trackers (limits learned from the partner's InitFC)
   - TX/RX queues for both DLLPs and TLPs
   - The FC Init FSM (`fc_init_tick()`)
   - TLP construction helpers (`enqueue_mem_write`, `enqueue_mem_read`, `enqueue_completion`)
   - Credit-gated transmit (`try_transmit_tlp`)

   The device is **silent** — it produces no console output. All rendering
   is done externally by the Diagram class.

3. **`include/diagram.h`** + **`src/diagram.cpp`** — The `Diagram` class
   renders the ASCII art visualization. It knows nothing about device state
   machines — it simply draws arrows, boxes, gauges, and banners based on
   the packet and credit data it receives. Dual output (colored terminal +
   plain log file) is handled via a single `emit()` method.

4. **`src/main.cpp`** — The simulation scenario. Creates the two devices,
   runs the FC handshake cycle by cycle, then drives TLP traffic through
   the five phases. A small `transfer_and_render_dllps()` helper handles
   the "extract DLLPs from sender, render them, deliver to receiver" pattern.

5. **`standalone/pcie_fc_sim.cpp`** — A self-contained single-file version
   of the entire simulator, useful for quick builds without multi-file setup.

---

## Building & Running

```bash
make        # compile all .cpp files → build/pcie_fc_sim
make run    # build + execute (output written to output/)
make clean  # remove build/ artifacts and output/pcie_sim.log
```

Requires a C++17-capable compiler (GCC 7+, Clang 5+, MSVC 19.14+).

The simulation produces `output/pcie_sim.log` — a plain-text version of the
diagram without ANSI escape codes, suitable for archival or sharing.

---

## Example Output (excerpt)

```
          |  InitFC1-P   [HdrCr=8 DataCr=32]                                  |
          |  InitFC1-NP  [HdrCr=4 DataCr=16]                                  |
          |  InitFC1-Cpl [HdrCr=0 DataCr=0] (infinite)                        |
          |---------------------------------------------------------------->> |
          |                                                                   |
          |                                   InitFC1-P   [HdrCr=4 DataCr=8]  |
          |                                   InitFC1-NP  [HdrCr=2 DataCr=4]  |
          |                                   InitFC1-Cpl [HdrCr=4 DataCr=8]  |
          | <<----------------------------------------------------------------|
```

```
          |  +-- MWr32  Seq#0  [P] -----------------------------------+       |
          |  | DW0: Fmt=2 Type=00000 TC=0 Len=4DW                    |        |
          |  | DW1: ReqID=0x0100 Tag=0 BE=fh/fh                     |         |
          |  | DW2: Addr=0x00001000                                  |         |
          |  | Data: DEADBEEF 12345678 A5A5A5A5 FEEDFACE             |         |
          |  +-------------------------------------------------------+        |
          |=====================[P: H:1/4 D:1/8]========================>>    |
```

---

## References

- **PCI Express Base Specification, Rev 5.0** — Sections 2.5–2.7 (TLP format),
  Section 3.4 (Flow Control), Section 3.5 (Data Link Layer)
- **MindShare: PCI Express Technology 3.0** — Chapters 7 (Flow Control) and 8
  (Transaction Layer)
