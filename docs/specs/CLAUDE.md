# docs/specs/CLAUDE.md

This directory holds **public, vendor-neutral specifications** that describe
conventions HelixScreen participates in alongside other tools. They are
written for third-party adopters (firmware vendors, slicers, scales,
spool-tracking tools), not as HelixScreen implementation docs.

## Current specs

- [`filament_slots.md`](filament_slots.md) — the `lane_data` Moonraker-DB
  filament slot metadata convention. Written by AFC and HelixScreen, read by
  OrcaSlicer 2.3.2+ (verified unchanged through 2.4.0-beta). Note: Happy Hare
  is read by OrcaSlicer from the live `mmu` object, not this namespace.

## Style

Specs in this directory should:

- Credit originators and cite authoritative references
- Avoid HelixScreen-internal implementation details (link to `docs/devel/` for those)
- Document schemas, field semantics, and conformance expectations
- Use JSON examples liberally
- Include a changelog section
