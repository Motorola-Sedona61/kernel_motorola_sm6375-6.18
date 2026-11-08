# Fogos ACK 6.18 port

This is the complete kernel source for the Fogos Android boot checkpoint r4.
The ACK configuration remains separate from the Qualcomm/Motorola modules.

- Linux release: 6.18.33-android17-6-sedona.
- ACK upstream checkpoint: b97c15a2719d (full revision in SOURCE_REVISION.json).
- Local pre-port checkpoint: 7f9d3f1c3fa649befb6b898a97e719ee206ba6b0.
- Board configuration: arch/arm64/configs/fogos_gki.fragment.
- Toolchain: Android clang-r596125 and Rust 1.91.1.

The private build repository documents the build wrappers, source provenance,
module policy and device-tested progression. Vendor drivers are maintained in
the matching private vendor repository. Retain the existing Fogos 6.1 DT and
firmware interfaces; do not mix AudioReach/SPF with the legacy APR/Q6 stack.

r4 boots Android on the user's device. The user reports lag, dead audio and
heating. Those issues remain under investigation; successful boot is not a
claim that all peripherals or power management work.

Publication uses a complete source snapshot because the local ACK object store
contains 4.75 GiB of upstream history. Exact upstream and local checkpoint IDs
are retained; the original full history remains in the local development repo.
