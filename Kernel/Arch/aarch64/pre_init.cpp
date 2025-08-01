/*
 * Copyright (c) 2023, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Types.h>
#include <Kernel/Arch/aarch64/CPU.h>
#include <Kernel/Sections.h>

#include <LibELF/Relocation.h>

// We arrive here from boot.S with the MMU disabled and in an unknown exception level (EL).
// The kernel is linked at the virtual address, so we have to be really carefull when accessing
// global variables, as the MMU is not yet enabled.

// FIXME: This should probably be shared with the Prekernel.

namespace Kernel {

extern "C" [[noreturn]] void init();

static UNMAP_AFTER_INIT PhysicalPtr physical_load_base()
{
    PhysicalPtr physical_load_base;

    asm volatile(
        "adrp %[physical_load_base], start_of_kernel_image\n"
        : [physical_load_base] "=r"(physical_load_base));

    return physical_load_base;
}

static UNMAP_AFTER_INIT PhysicalPtr dynamic_section_addr()
{
    PhysicalPtr dynamic_section_addr;

    // Use adrp+add explicitly to prevent a GOT load.
    asm volatile(
        "adrp %[dynamic_section_addr], _DYNAMIC\n"
        "add %[dynamic_section_addr], %[dynamic_section_addr], :lo12:_DYNAMIC\n"
        : [dynamic_section_addr] "=r"(dynamic_section_addr));

    return dynamic_section_addr;
}

extern "C" [[noreturn]] void pre_init(PhysicalPtr flattened_devicetree_paddr);
extern "C" [[noreturn]] void pre_init(PhysicalPtr flattened_devicetree_paddr)
{
    // Apply relative relocations as if we were running at KERNEL_MAPPING_BASE.
    // This means that all global variables must be accessed with adjust_by_mapping_base, since we are still running identity mapped.
    // Otherwise, we would have to relocate twice: once while running identity mapped, and again when we enable the MMU.
    if (!ELF::perform_relative_relocations(physical_load_base(), KERNEL_MAPPING_BASE, dynamic_section_addr()))
        panic_without_mmu("Failed to perform relative relocations"sv);

    // We want to drop to EL1 as soon as possible, because that is the
    // exception level the kernel should run at.
    initialize_exceptions();

    // Next step is to set up page tables and enable the MMU.
    Memory::init_page_tables(flattened_devicetree_paddr);

    // At this point the MMU is enabled, physical memory is identity mapped,
    // and the kernel is also mapped into higher virtual memory. However we are still executing
    // from the physical memory address, so we have to jump to the kernel in high memory. We also need to
    // switch the stack pointer to high memory, such that we can unmap the identity mapping.

    // Continue execution at high virtual address.
    asm volatile(
        "adrp x0, 1f \n"
        "add x0, x0, :lo12:1f \n"
        "add x0, x0, %[base] \n"
        "br x0 \n"
        "1: \n" ::[base] "r"(g_boot_info.physical_to_virtual_offset)
        : "x0");

    // Add kernel_mapping_base to the stack pointer, such that it is also using the mapping
    // in high virtual memory.
    asm volatile(
        "mov x0, %[base] \n"
        "add sp, sp, x0 \n" ::[base] "r"(g_boot_info.physical_to_virtual_offset)
        : "x0");

    // We can now unmap the identity map as everything is running in high virtual memory at this point.
    Memory::unmap_identity_map();

    // Clear the frame pointer (x29) and link register (x30) to make sure the kernel cannot backtrace
    // into this code, and jump to actual init function in the kernel.
    register FlatPtr const x0 asm("x0") = bit_cast<FlatPtr>(&g_boot_info);
    asm volatile(
        "mov x29, xzr \n"
        "mov x30, xzr \n"
        "b init \n" ::"r"(x0));

    VERIFY_NOT_REACHED();
}

}
