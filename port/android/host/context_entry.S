/* The context entry the Android rewrite reaches through key page entry 8 (blr x16): x16 = this thread's slot block,
   read from bionic's TLS_SLOT_APP (thread pointer + 16). Every other register and the flags are unchanged. */
    .text
    .globl runtime_android_context_entry
    .hidden runtime_android_context_entry
    .type runtime_android_context_entry, %function
    .p2align 2
runtime_android_context_entry:
    mrs x16, tpidr_el0
    ldr x16, [x16, #16]
    ret
    .size runtime_android_context_entry, .-runtime_android_context_entry
    .section .note.GNU-stack,"",%progbits
