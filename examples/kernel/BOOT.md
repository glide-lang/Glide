# Booting the Glide kernel (x86_64 + Limine + QEMU)

This is the P0 proof: a freestanding kernel written almost entirely in Glide.
The only non-Glide files are `linker.ld` (linker layout), `limine.conf`
(bootloader config), and a ~6-line `c_raw!` block in `kmain.glide` holding the
Limine magic markers. Everything else — port I/O, serial driver, entry — is
Glide.

## 1. Build the kernel ELF

The compiler needs freestanding support (this branch). Build with it:

    glide build kmain.glide -o kernel.elf

`kind: "kernel"` in `glide.glide` puts the compiler in freestanding mode and
links a bare-metal ELF with zig against `linker.ld`. Verify:

    readelf -h kernel.elf      # Entry point address: 0xffffffff800010b0 (kmain)
    readelf -S kernel.elf      # .limine_requests, .text, .rodata present

## 2. Make a bootable ISO (needs `xorriso` + the Limine binaries)

    # one-time: fetch + build the limine host tool
    git clone https://github.com/limine-bootloader/limine.git \
        --branch=v8.x-binary --depth=1
    make -C limine

    # stage the ISO tree
    mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
    cp kernel.elf                       iso_root/boot/kernel
    cp limine.conf                      iso_root/boot/limine/
    cp limine/limine-bios.sys \
       limine/limine-bios-cd.bin \
       limine/limine-uefi-cd.bin        iso_root/boot/limine/
    cp limine/BOOTX64.EFI               iso_root/EFI/BOOT/
    cp limine/BOOTIA32.EFI              iso_root/EFI/BOOT/

    # build the hybrid BIOS+UEFI ISO
    xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_root -o glide-kernel.iso

    # install the BIOS stage to the ISO
    ./limine/limine bios-install glide-kernel.iso

## 3. Boot in QEMU

    qemu-system-x86_64 -M q35 -m 256M -cdrom glide-kernel.iso -serial stdio

`-serial stdio` pipes COM1 to your terminal. Success looks like:

    Hello from a Glide kernel!

then the CPU halts (the kernel sits in a `hlt` loop).

## Notes

- On Windows, the ISO + QEMU steps are easiest from WSL or with QEMU/xorriso
  installed natively. The kernel ELF itself builds fine on the Windows host.
- SSE stays enabled in the build (zig's compiler_rt won't compile with
  `-mno-sse`). Limine hands off in long mode with SSE usable. The red zone is
  disabled (`-mno-red-zone`) for interrupt safety.
