CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude

SRCDIR   := src
INCDIR   := include
BUILDDIR := build
OUTDIR   := output

TARGET   := $(BUILDDIR)/pcie_fc_sim
SRCS     := $(SRCDIR)/main.cpp $(SRCDIR)/pcie_device.cpp $(SRCDIR)/diagram.cpp
HDRS     := $(wildcard $(INCDIR)/*.h)
OBJS     := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp $(HDRS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

run: $(TARGET) | $(OUTDIR)
	cd $(OUTDIR) && ../$(TARGET)

$(OUTDIR):
	mkdir -p $(OUTDIR)

clean:
	rm -rf $(BUILDDIR) $(OUTDIR)/pcie_sim.log
