// SPDX-License-Identifier: GPL-2.0
#include <linux/static_call.h>
#include <linux/memory.h>
#include <linux/bug.h>
#include <asm/text-patching.h>

enum insn_type {
	CALL = 0, /* site call */
	NOP = 1,  /* site cond-call */
	JMP = 2,  /* tramp / site tail-call */
	RET = 3,  /* tramp / site cond-tail-call */
};

/*
 * ud1 %esp, %ecx - a 3 byte #UD that is unique to trampolines, chosen such
 * that there is no false-positive trampoline identification while also being a
 * speculation stop.
 */
static const u8 tramp_ud[] = { 0x0f, 0xb9, 0xcc };

/*
 * data16 data16 xorq %rax, %rax - a single 5 byte instruction that clears %rax
 * The REX.W cancels the effect of any data16.
 */
static const u8 xor5rax[] = { 0x66, 0x66, 0x48, 0x31, 0xc0 };

static const u8 retinsn[] = { RET_INSN_OPCODE, 0xcc, 0xcc, 0xcc, 0xcc };

static void __ref __static_call_transform(void *insn, enum insn_type type, void *func)
{
	const void *emulate = NULL;
	int size = CALL_INSN_SIZE;
	const void *code;

	switch (type) {
	case CALL:
		code = text_gen_insn(CALL_INSN_OPCODE, insn, func);
		if (func == &__static_call_return0) {
			emulate = code;
			code = &xor5rax;
		}

		break;

	case NOP:
		code = x86_nops[5];
		break;

	case JMP:
		code = text_gen_insn(JMP32_INSN_OPCODE, insn, func);
		break;

	case RET:
		if (cpu_feature_enabled(X86_FEATURE_RETHUNK))
			code = text_gen_insn(JMP32_INSN_OPCODE, insn, &__x86_return_thunk);
		else
			code = &retinsn;
		break;
	}

	if (memcmp(insn, code, size) == 0)
		return;

	if (unlikely(system_state == SYSTEM_BOOTING))
		return text_poke_early(insn, code, size);

	text_poke_bp(insn, code, size, emulate);
}

static void __static_call_validate(void *insn, bool tail, bool tramp)
{
	u8 opcode = *(u8 *)insn;

	if (tramp && memcmp(insn+5, tramp_ud, 3)) {
		pr_err("trampoline signature fail");
		BUG();
	}

	if (tail) {
		if (opcode == JMP32_INSN_OPCODE ||
		    opcode == RET_INSN_OPCODE)
			return;
	} else {
		if (opcode == CALL_INSN_OPCODE ||
		    !memcmp(insn, x86_nops[5], 5) ||
		    !memcmp(insn, xor5rax, 5))
			return;
	}

	/*
	 * If we ever trigger this, our text is corrupt, we'll probably not live long.
	 */
	pr_err("unexpected static_call insn opcode 0x%x at %pS\n", opcode, insn);
	BUG();
}

static inline enum insn_type __sc_insn(bool null, bool tail)
{
	/*
	 * Encode the following table without branches:
	 *
	 *	tail	null	insn
	 *	-----+-------+------
	 *	  0  |   0   |  CALL
	 *	  0  |   1   |  NOP
	 *	  1  |   0   |  JMP
	 *	  1  |   1   |  RET
	 */
	return 2*tail + null;
}

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	mutex_lock(&text_mutex);

	if (tramp) {
		__static_call_validate(tramp, true, true);
		__static_call_transform(tramp, __sc_insn(!func, true), func);
	}

	if (IS_ENABLED(CONFIG_HAVE_STATIC_CALL_INLINE) && site) {
		__static_call_validate(site, tail, false);
		__static_call_transform(site, __sc_insn(!func, tail), func);
	}

	mutex_unlock(&text_mutex);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);

#ifdef CONFIG_RETPOLINE
/*
 * This is called by apply_returns() to fix up static call trampolines,
 * specifically ARCH_DEFINE_STATIC_CALL_NULL_TRAMP which is recorded as
 * having a return trampoline.
 *
 * The problem is that static_call() is available before determining
 * X86_FEATURE_RETHUNK and, by implication, running alternatives.
 *
 * This means that __static_call_transform() above can have overwritten the
 * return trampoline and we now need to fix things up to be consistent.
 */
bool __static_call_fixup(void *tramp, u8 op, void *dest)
{
	if (memcmp(tramp+5, tramp_ud, 3)) {
		/* Not a trampoline site, not our problem. */
		return false;
	}

	if (op == RET_INSN_OPCODE || dest == &__x86_return_thunk)
		__static_call_transform(tramp, RET, NULL);

	return true;
}
#endif
