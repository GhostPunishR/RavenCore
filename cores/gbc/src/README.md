# GBC core boundary

The current RavenEmu GB implementation already contains the CGB hardware mode
and selects it from the cartridge header. This directory is the extraction
boundary for GBC-specific CPU/memory/PPU/APU/cartridge/DMA work. Code moves here
only when parity tests prove that the split does not change emulation behavior.
