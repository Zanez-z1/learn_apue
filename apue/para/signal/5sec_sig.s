	.file	"5sec_sig.c"
	.text
	.type	alrm_handler, @function
alrm_handler:
.LFB22:
	.cfi_startproc
	movl	$0, loop(%rip)
	ret
	.cfi_endproc
.LFE22:
	.size	alrm_handler, .-alrm_handler
	.section	.rodata.str1.1,"aMS",@progbits,1
.LC0:
	.string	"%lld\n"
	.text
	.globl	main
	.type	main, @function
main:
.LFB23:
	.cfi_startproc
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movl	$5, %edi
	call	alarm@PLT
	leaq	alrm_handler(%rip), %rsi
	movl	$14, %edi
	call	signal@PLT
	cmpl	$0, loop(%rip)
	je	.L3
	.p2align 1
.L4:
	jmp	.L4
.L3:
	movl	$0, %esi
	leaq	.LC0(%rip), %rdi
	movl	$0, %eax
	call	printf@PLT
	movl	$0, %edi
	call	exit@PLT
	.cfi_endproc
.LFE23:
	.size	main, .-main
	.data
	.align 4
	.type	loop, @object
	.size	loop, 4
loop:
	.long	1
	.ident	"GCC: (GNU) 16.2.1 20260810"
	.section	.note.GNU-stack,"",@progbits
