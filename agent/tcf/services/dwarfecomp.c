/*******************************************************************************
 * Copyright (c) 2011, 2013 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Transformation of DWARF expressions to a portable form.
 *
 * Functions in this module use exceptions to report errors, see exceptions.h
 */

#include <tcf/config.h>

#if ENABLE_ELF && ENABLE_DebugContext

#include <assert.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/cache.h>
#include <tcf/services/dwarf.h>
#include <tcf/services/dwarfreloc.h>
#include <tcf/services/dwarfecomp.h>
#include <tcf/services/elf-loader.h>
#include <tcf/services/elf-symbols.h>
#include <tcf/services/stacktrace.h>

typedef struct JumpInfo {
    U1_T op;
    I2_T delta;
    I2_T jump_offs;
    size_t size;
    size_t src_pos;
    size_t dst_pos;
    struct JumpInfo * next;
} JumpInfo;

static U1_T * buf = NULL;
static size_t buf_pos = 0;
static size_t buf_max = 0;
static JumpInfo * jumps = NULL;
static DWARFExpressionInfo * expr = NULL;
static size_t expr_pos = 0;
static Context * expr_ctx = NULL;
static int expr_frame = 0;
static ContextAddress expr_code_addr = 0;
static ContextAddress expr_code_size = 0;
static int expr_big_endian = 0;

static void add(unsigned n) {
    if (buf_pos >= buf_max) {
        buf_max *= 2;
        buf = (U1_T *)tmp_realloc(buf, buf_max);
    }
    buf[buf_pos++] = (U1_T)n;
}

static void copy(size_t n) {
    while (n > 0) {
        if (expr_pos >= expr->expr_size) exception(ERR_INV_DWARF);
        add(expr->expr_addr[expr_pos++]);
        n--;
    }
}

static void copy_leb128(void) {
    for (;;) {
        U1_T n = expr->expr_addr[expr_pos++];
        add(n);
        if ((n & 0x80) == 0) break;
    }
}

static void add_uleb128(U8_T x) {
    for (;;) {
        U1_T n = (U1_T)(x & 0x7Fu);
        x = x >> 7;
        if (x == 0) {
            add(n);
            break;
        }
        add(n | 0x80u);
    }
}

static U4_T read_u4leb128(void) {
    U4_T v = 0;
    int i = 0;
    for (;; i += 7) {
        U1_T n = expr->expr_addr[expr_pos++];
        v |= (U4_T)(n & 0x7f) << i;
        if ((n & 0x80) == 0) break;
    }
    return v;
}

static I8_T read_i8leb128(void) {
    U8_T v = 0;
    int i = 0;
    for (;; i += 7) {
        U1_T n = expr->expr_addr[expr_pos++];
        v |= (U8_T)(n & 0x7Fu) << i;
        if ((n & 0x80) == 0) {
            v |= -(I8_T)(n & 0x40) << i;
            break;
        }
    }
    return (I8_T)v;
}

static void add_sleb128(I8_T x) {
    for (;;) {
        U1_T n = (U1_T)(x & 0x7Fu);
        x = x >> 7;
        if ((x == 0 && (n & 0x40) == 0) ||
            (x == -1 && (n & 0x40) != 0)) {
            add(n);
            break;
        }
        add(n | 0x80u);
    }
}

static void set_u2(size_t pos, U2_T v) {
    if (expr_big_endian) {
        buf[pos++] = (U1_T)((v >> 8) & 0xffu);
        buf[pos++] = (U1_T)(v & 0xffu);
    }
    else {
        buf[pos++] = (U1_T)(v & 0xffu);
        buf[pos++] = (U1_T)((v >> 8) & 0xffu);
    }
}

static void add_expression(DWARFExpressionInfo * info);

static int get_num_prop(ObjectInfo * obj, U2_T at, U8_T * res) {
    Trap trap;
    PropertyValue v;

    if (!set_trap(&trap)) return 0;
    read_and_evaluate_dwarf_object_property(expr_ctx, STACK_NO_FRAME, obj, at, &v);
    *res = get_numeric_property_value(&v);
    clear_trap(&trap);
    return 1;
}

static void op_addr(void) {
    ContextAddress addr = 0;
    ELF_Section * section = NULL;
    U8_T pos = 0;
    int rt = 0;

    expr_pos++;
    pos = expr->expr_addr + expr_pos - (U1_T *)expr->section->data;
    dio_EnterSection(&expr->object->mCompUnit->mDesc, expr->section, pos);
    switch (expr->object->mCompUnit->mDesc.mAddressSize) {
    case 2: {
        U2_T x = dio_ReadU2();
        drl_relocate_in_context(expr_ctx, expr->section, pos, &x, sizeof(x), &section, &rt);
        addr = x;
        break;
    }
    case 4: {
        U4_T x = dio_ReadU4();
        drl_relocate_in_context(expr_ctx, expr->section, pos, &x, sizeof(x), &section, &rt);
        addr = x;
        break;
    }
    case 8: {
        U8_T x = dio_ReadU8();
        drl_relocate_in_context(expr_ctx, expr->section, pos, &x, sizeof(x), &section, &rt);
        addr = x;
        break;
    }
    default:
        str_exception(ERR_INV_DWARF, "Invalid data size");
        return;
    }
    expr_pos += (size_t)(dio_GetPos() - pos);
    dio_ExitSection();
    if (expr_pos < expr->expr_size && expr->expr_addr[expr_pos] == OP_GNU_push_tls_address) {
        /* Bug in some versions of GCC: OP_addr used instead of OP_const, use link-time value */
    }
    else if (!rt) {
        addr = elf_map_to_run_time_address(expr_ctx, expr->object->mCompUnit->mFile, section, addr);
        if (errno) str_exception(errno, "Cannot get object run-time address");
    }
    add(OP_constu);
    add_uleb128(addr);
}

static ObjectInfo * get_parent_function(ObjectInfo * info) {
    while (info != NULL) {
        switch (info->mTag) {
        case TAG_global_subroutine:
        case TAG_inlined_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
            return info;
        }
        info = get_dwarf_parent(info);
    }
    return NULL;
}

static void add_fbreg_expression(DWARFExpressionInfo * info, I8_T offs) {
    switch (*info->expr_addr) {
    case OP_reg:
        add(OP_basereg);
        {
            unsigned i = 1;
            while (i < info->object->mCompUnit->mDesc.mAddressSize + 1u) {
                add(info->expr_addr[i++]);
            }
        }
        add_sleb128(offs);
        return;
    case OP_regx:
        add(OP_bregx);
        {
            unsigned i = 1;
            for (;;) {
                U1_T n = info->expr_addr[i++];
                add(n);
                if ((n & 0x80) == 0) break;
            }
        }
        add_sleb128(offs);
        return;
    case OP_reg0:
    case OP_reg1:
    case OP_reg2:
    case OP_reg3:
    case OP_reg4:
    case OP_reg5:
    case OP_reg6:
    case OP_reg7:
    case OP_reg8:
    case OP_reg9:
    case OP_reg10:
    case OP_reg11:
    case OP_reg12:
    case OP_reg13:
    case OP_reg14:
    case OP_reg15:
    case OP_reg16:
    case OP_reg17:
    case OP_reg18:
    case OP_reg19:
    case OP_reg20:
    case OP_reg21:
    case OP_reg22:
    case OP_reg23:
    case OP_reg24:
    case OP_reg25:
    case OP_reg26:
    case OP_reg27:
    case OP_reg28:
    case OP_reg29:
    case OP_reg30:
    case OP_reg31:
        add(OP_breg0 + (*info->expr_addr - OP_reg0));
        add_sleb128(offs);
        return;
    }
    add_expression(info);
    if (offs == 0) return;
    add(OP_consts);
    add_sleb128(offs);
    add(OP_add);
}

static void add_code_range(DWARFExpressionInfo * info) {
    if (info->code_size) {
        if (expr_code_size) {
            if (info->code_addr > expr_code_addr) {
                U8_T d = info->code_addr - expr_code_addr;
                assert(expr_code_size > d);
                expr_code_addr += d;
                expr_code_size -= d;
            }
            if (info->code_addr + info->code_size < expr_code_addr + expr_code_size) {
                U8_T d = (expr_code_addr + expr_code_size) - (info->code_addr + info->code_size);
                assert(expr_code_size > d);
                expr_code_size -= d;
            }
        }
        else {
            expr_code_addr = info->code_addr;
            expr_code_size = info->code_size;
        }
    }
}

static void add_expression_list(DWARFExpressionInfo * info, int fbreg, I8_T offs) {
    int peer_version = 1;
    Channel * c = cache_channel();
    if (c != NULL) {
        int i;
        peer_version = 0;
        for (i = 0; i < c->peer_service_cnt; i++) {
            char * nm = c->peer_service_list[i];
            if (strcmp(nm, "SymbolsProxyV1") == 0) peer_version = 1;
        }
    }

    if (info->code_size > 0 && peer_version == 0) {
        /* The peer does not support OP_TCF_switch */
        uint64_t pc = 0;
        StackFrame * frame = NULL;
        if (expr_frame == STACK_NO_FRAME) str_exception(ERR_INV_CONTEXT, "Need stack frame");
        if (get_frame_info(expr_ctx, expr_frame, &frame) < 0) exception(errno);
        if (read_reg_value(frame, get_PC_definition(expr_ctx), &pc) < 0) exception(errno);
        while (info != NULL && info->code_size > 0 && (info->code_addr > pc || pc - info->code_addr >= info->code_size)) {
            info = info->next;
        }
        if (info == NULL) {
            str_exception(ERR_OTHER, "Object is not available at this location in the code");
        }
        if (!fbreg) add_expression(info);
        else add_fbreg_expression(info, offs);
        add_code_range(info);
    }
    else if (info->code_size > 0) {
        size_t switch_pos;
        RegisterDefinition * pc = get_PC_definition(expr_ctx);

        if (pc == NULL || pc->dwarf_id < 0)
            str_exception(ERR_INV_DWARF, "Invalid PC register definition");

        add(OP_bregx);
        add_uleb128(pc->dwarf_id);
        add_uleb128(0);
        add(OP_TCF_switch);
        add(0);
        add(0);
        switch_pos = buf_pos;
        while (info != NULL) {
            if (expr_code_size == 0 || info->code_size == 0 ||
                    (expr_code_addr + expr_code_size > info->code_addr &&
                    expr_code_addr < info->code_addr + info->code_size)) {
                size_t case_pos;
                U8_T org_expr_code_addr = expr_code_addr;
                U8_T org_expr_code_size = expr_code_size;
                add_code_range(info);
                add(0);
                add(0);
                case_pos = buf_pos;
                add_uleb128(info->code_addr);
                add_uleb128(info->code_size);
                if (!fbreg) add_expression(info);
                else add_fbreg_expression(info, offs);
                expr_code_addr = org_expr_code_addr;
                expr_code_size = org_expr_code_size;
                set_u2(case_pos - 2, buf_pos - case_pos);
                if (info->code_size == 0) break;
            }
            info = info->next;
        }
        add(0);
        add(0);
        if (buf_pos - switch_pos > 0xffff)
            str_exception(ERR_INV_DWARF, "Location expression is too large");
        set_u2(switch_pos - 2, buf_pos - switch_pos);
    }
    else if (fbreg) {
        add_fbreg_expression(info, offs);
    }
    else {
        assert(offs == 0);
        add_expression(info);
    }
}

static void op_fbreg(void) {
    PropertyValue fp;
    DWARFExpressionInfo * info;
    ObjectInfo * parent = get_parent_function(expr->object);
    I8_T offs = 0;

    expr_pos++;
    offs = read_i8leb128();
    memset(&fp, 0, sizeof(fp));
    if (parent == NULL) str_exception(ERR_INV_DWARF, "OP_fbreg: no parent function");
    read_dwarf_object_property(expr_ctx, STACK_NO_FRAME, parent, AT_frame_base, &fp);
    dwarf_get_expression_list(&fp, &info);
    add_expression_list(info, 1, offs);
}

static void op_implicit_pointer(void) {
    Trap trap;
    PropertyValue pv;
    U1_T op = expr->expr_addr[expr_pos];
    CompUnit * unit = expr->object->mCompUnit;
    int arg_size = unit->mDesc.m64bit ? 8 : 4;
    ObjectInfo * ref_obj = NULL;
    ContextAddress ref_id = 0;
    U8_T offset = 0;
    U8_T dio_pos = 0;

    expr_pos++;
    if (op == OP_GNU_implicit_pointer && unit->mDesc.mVersion < 3) arg_size = unit->mDesc.mAddressSize;
    dio_pos = expr->expr_addr + expr_pos - (U1_T *)expr->section->data;
    dio_EnterSection(&expr->object->mCompUnit->mDesc, expr->section, dio_pos);
    ref_id = dio_ReadAddressX(NULL, arg_size);
    offset = dio_ReadU8LEB128();
    expr_pos += (size_t)(dio_GetPos() - dio_pos);
    dio_ExitSection();

    ref_obj = find_object(get_dwarf_cache(unit->mFile), ref_id);
    if (ref_obj == NULL) str_exception(ERR_INV_DWARF, "OP_implicit_pointer: invalid object reference");

    memset(&pv, 0, sizeof(pv));
    if (set_trap(&trap)) {
        DWARFExpressionInfo * info = NULL;
        read_dwarf_object_property(expr_ctx, STACK_NO_FRAME, ref_obj, AT_location, &pv);
        dwarf_get_expression_list(&pv, &info);
        add_expression_list(info, 0, 0);
        if (offset != 0) {
            add(OP_TCF_offset);
            add_uleb128(offset);
        }
        clear_trap(&trap);
    }
    else if (trap.error == ERR_SYM_NOT_FOUND) {
        size_t i;
        union {
            U4_T u4;
            U8_T u8;
            U1_T arr[8];
        } buf;
        read_dwarf_object_property(expr_ctx, STACK_NO_FRAME, ref_obj, AT_const_value, &pv);
        switch (pv.mForm) {
        case FORM_SDATA:
        case FORM_UDATA:
            pv.mAddr = buf.arr;
            if (unit->mFile->elf64) {
                pv.mSize = 8;
                buf.u8 = pv.mValue;
            }
            else {
                pv.mSize = 4;
                buf.u4 = (U4_T)pv.mValue;
            }
            if (unit->mFile->byte_swap) swap_bytes(buf.arr, pv.mSize);
            break;
        }
        if (pv.mAddr == NULL) str_exception(ERR_INV_DWARF, "Invalid OP_GNU_implicit_pointer");
        if (offset != 0) {
            if (offset > pv.mSize) str_exception(ERR_INV_DWARF, "Invalid OP_GNU_implicit_pointer");
            pv.mSize -= (size_t)offset;
            pv.mAddr = (U1_T *)pv.mAddr + offset;
        }
        add(OP_implicit_value);
        add_uleb128(pv.mSize);
        for (i = 0; i < pv.mSize; i++) {
            add(*((U1_T *)pv.mAddr + i));
        }
    }
    else {
        exception(trap.error);
    }
}

static void op_push_tls_address(void) {
    U8_T addr = 0;
    expr_pos++;
    if (expr_pos == 1 && expr_pos < expr->expr_size) {
        /* This looks like a bug in GCC: offset sometimes is emitted after OP_GNU_push_tls_address */
        U1_T op = expr->expr_addr[expr_pos];
        switch (op) {
        case OP_const4u: copy(5); break;
        case OP_const8u: copy(9); break;
        case OP_constu:  add(op); expr_pos++; copy_leb128(); break;
        }
    }
    if (!context_has_state(expr_ctx)) str_exception(ERR_INV_CONTEXT,
        "Thread local variable, but context is not a thread");
    addr = get_tls_address(expr_ctx, expr->object->mCompUnit->mFile);
    if (addr != 0) {
        add(OP_constu);
        add_uleb128(addr);
        add(OP_add);
    }
}

static void op_call(void) {
    U8_T ref_id = 0;
    DIO_UnitDescriptor * desc = &expr->object->mCompUnit->mDesc;
    U8_T dio_pos = 0;
    U1_T opcode = 0;
    ObjectInfo * ref_obj = NULL;
    DWARFExpressionInfo * info = NULL;
    PropertyValue pv;

    opcode = expr->expr_addr[expr_pos++];
    dio_pos = expr->expr_addr + expr_pos - (U1_T *)expr->section->data;
    dio_EnterSection(desc, expr->section, dio_pos);
    switch (opcode) {
    case OP_call2:
        ref_id = desc->mSection->addr + desc->mUnitOffs + dio_ReadU2();
        break;
    case OP_call4:
        ref_id = desc->mSection->addr + desc->mUnitOffs + dio_ReadU4();
        break;
    case OP_call_ref:
        {
            ELF_Section * section = NULL;
            int size = desc->m64bit ? 8 : 4;
            if (desc->mVersion < 3) size = desc->mAddressSize;
            ref_id = dio_ReadAddressX(&section, size);
        }
        break;
    }
    expr_pos += (size_t)(dio_GetPos() - dio_pos);
    dio_ExitSection();

    ref_obj = find_object(get_dwarf_cache(expr->object->mCompUnit->mFile), ref_id);
    if (ref_obj == NULL) str_exception(ERR_INV_DWARF, "Invalid reference in OP_call");
    read_dwarf_object_property(expr_ctx, STACK_NO_FRAME, ref_obj, AT_location, &pv);
    dwarf_get_expression_list(&pv, &info);
    add_expression_list(info, 0, 0);
}

static void op_entry_value(void) {
    size_t size = 0;
    size_t size_pos = 0;
    DWARFExpressionInfo info;

    add(expr->expr_addr[expr_pos++]);
    size_pos = buf_pos;
    add(0);
    add(0);
    add(0);
    size = read_u4leb128();
    memset(&info, 0, sizeof(info));
    info.object = expr->object;
    info.section = expr->section;
    info.expr_addr = expr->expr_addr + expr_pos;
    info.expr_size = size;
    add_expression(&info);
    expr_pos += size;
    size = buf_pos - size_pos - 3;
    buf[size_pos++] = (U1_T)((size & 0x7f) | 0x80);
    buf[size_pos++] = (U1_T)(((size >> 7) & 0x7f) | 0x80);
    buf[size_pos++] = (U1_T)((size >> 14) & 0x7f);
}

static void adjust_jumps(void) {
    JumpInfo * i = jumps;
    while (i != NULL) {
        if (i->op == OP_bra || i->op == OP_skip) {
            int delta = 0;
            JumpInfo * j = jumps;
            while (j != NULL) {
                if (i->jump_offs > 0) {
                    if (j->src_pos > i->src_pos && j->src_pos < i->src_pos + i->jump_offs) {
                        delta += j->delta;
                    }
                }
                else {
                    if (j->src_pos > i->src_pos + i->jump_offs && j->src_pos < i->src_pos) {
                        delta -= j->delta;
                    }
                }
                j = j->next;
            }
            if (delta != 0) {
                U2_T new_offs = (U2_T)(i->jump_offs + delta);
                set_u2(i->dst_pos + 1, new_offs);
            }
        }
        i = i->next;
    }
}

static void check_frame(void) {
    if (expr_frame != STACK_NO_FRAME) return;
    str_exception(ERR_OTHER, "Object location is relative to a stack frame");
}

static void add_expression(DWARFExpressionInfo * info) {
    DWARFExpressionInfo * org_expr = expr;
    size_t org_expr_pos = expr_pos;
    JumpInfo * org_jumps = jumps;

    expr = info;
    expr_pos = 0;
    jumps = NULL;
    if (expr->object->mCompUnit->mFile->big_endian != expr_big_endian) {
        str_exception(ERR_OTHER, "Invalid endianness of location expression");
    }
    while (expr_pos < info->expr_size) {
        size_t op_src_pos = expr_pos;
        size_t op_dst_pos = buf_pos;
        U1_T op = info->expr_addr[expr_pos];
        switch (op) {
        case OP_const1u:
        case OP_const1s:
        case OP_pick:
        case OP_deref_size:
        case OP_xderef_size:
            copy(2);
            break;
        case OP_const:
        case OP_reg:
            copy(1 + info->object->mCompUnit->mDesc.mAddressSize);
            break;
        case OP_basereg:
            check_frame();
            copy(1 + info->object->mCompUnit->mDesc.mAddressSize);
            break;
        case OP_const2u:
        case OP_const2s:
            copy(3);
            break;
        case OP_const4u:
        case OP_const4s:
            copy(5);
            break;
        case OP_const8u:
        case OP_const8s:
            copy(9);
            break;
        case OP_breg0:
        case OP_breg1:
        case OP_breg2:
        case OP_breg3:
        case OP_breg4:
        case OP_breg5:
        case OP_breg6:
        case OP_breg7:
        case OP_breg8:
        case OP_breg9:
        case OP_breg10:
        case OP_breg11:
        case OP_breg12:
        case OP_breg13:
        case OP_breg14:
        case OP_breg15:
        case OP_breg16:
        case OP_breg17:
        case OP_breg18:
        case OP_breg19:
        case OP_breg20:
        case OP_breg21:
        case OP_breg22:
        case OP_breg23:
        case OP_breg24:
        case OP_breg25:
        case OP_breg26:
        case OP_breg27:
        case OP_breg28:
        case OP_breg29:
        case OP_breg30:
        case OP_breg31:
            check_frame();
            add(op);
            expr_pos++;
            copy_leb128();
            break;
        case OP_regx:
        case OP_constu:
        case OP_consts:
        case OP_plus_uconst:
        case OP_piece:
            add(op);
            expr_pos++;
            copy_leb128();
            break;
        case OP_bra:
        case OP_skip:
            {
                U2_T x0 = info->expr_addr[expr_pos + 1];
                U2_T x1 = info->expr_addr[expr_pos + 2];
                U2_T offs = expr_big_endian ? (x0 << 8) | x1 : x0 | (x1 << 8);
                if (offs != 0) {
                    JumpInfo * i = (JumpInfo *)tmp_alloc_zero(sizeof(JumpInfo));
                    i->op = op;
                    i->jump_offs = (I2_T)offs;
                    i->src_pos = op_src_pos;
                    i->dst_pos = op_dst_pos;
                    i->next = jumps;
                    jumps = i;
                    copy(3);
                }
            }
            break;
        case OP_bregx:
            check_frame();
            add(op);
            expr_pos++;
            copy_leb128();
            copy_leb128();
            break;
        case OP_bit_piece:
            add(op);
            expr_pos++;
            copy_leb128();
            copy_leb128();
            break;
        case OP_implicit_value:
            {
                unsigned i = 0;
                size_t j = expr_pos + 1u;
                size_t size = 0;
                for (;; i += 7) {
                    U1_T n = info->expr_addr[j++];
                    size |= (n & 0x7Fu) << i;
                    if ((n & 0x80) == 0) break;
                }
                copy(j + size - expr_pos);
            }
            break;
        case OP_fbreg:
            check_frame();
            op_fbreg();
            break;
        case OP_addr:
            op_addr();
            break;
        case OP_implicit_pointer:
        case OP_GNU_implicit_pointer:
            op_implicit_pointer();
            break;
        case OP_form_tls_address:
        case OP_GNU_push_tls_address:
            op_push_tls_address();
            break;
        case OP_call2:
        case OP_call4:
        case OP_call_ref:
            op_call();
            break;
        case OP_GNU_entry_value:
            check_frame();
            op_entry_value();
            break;
        case OP_GNU_regval_type:
            expr_pos++;
            {
                U8_T size = 0;
                U4_T reg = read_u4leb128();
                U4_T type = read_u4leb128();
                ObjectInfo * obj = find_object(
                    get_dwarf_cache(expr->object->mCompUnit->mFile),
                    expr->object->mCompUnit->mDesc.mSection->addr +
                    expr->object->mCompUnit->mDesc.mUnitOffs + type);
                if (obj == NULL) str_exception(ERR_INV_DWARF, "Invalid reference in OP_GNU_regval_type");
                if (!get_num_prop(obj, AT_byte_size, &size))str_exception(ERR_INV_DWARF, "Invalid reference in OP_GNU_regval_type");
                add(OP_regx);
                add_uleb128(reg);
                add(OP_piece);
                add_uleb128(size);
            }
            break;
        default:
            if (op >= OP_lo_user) {
                str_fmt_exception(ERR_OTHER, "Unsupported DWARF expression op 0x%02x", op);
            }
            add(op);
            expr_pos++;
            break;
        }
        if (buf_pos - op_dst_pos != expr_pos - op_src_pos) {
            JumpInfo * i = (JumpInfo *)tmp_alloc_zero(sizeof(JumpInfo));
            i->op = op;
            i->delta = (I2_T)(buf_pos - op_dst_pos) - (I2_T)(expr_pos - op_src_pos);
            i->src_pos = op_src_pos;
            i->dst_pos = op_dst_pos;
            i->next = jumps;
            jumps = i;
        }
    }
    adjust_jumps();
    expr = org_expr;
    expr_pos = org_expr_pos;
    jumps = org_jumps;
}

static void transform_expression(void * args) {
    add_expression_list((DWARFExpressionInfo *)args, 0, 0);
}

void dwarf_transform_expression(Context * ctx, int frame, DWARFExpressionInfo * info) {
    int error = 0;

    /* Save state - some expressions need to make recursive calls to symbols API */
    U1_T * org_buf = buf;
    size_t org_buf_pos = buf_pos;
    size_t org_buf_max = buf_max;
    JumpInfo * org_jumps = jumps;
    DWARFExpressionInfo * org_expr = expr;
    size_t org_expr_pos = expr_pos;
    Context * org_expr_ctx = expr_ctx;
    int org_expr_frame = expr_frame;
    ContextAddress org_expr_code_addr = expr_code_addr;
    ContextAddress org_expr_code_size = expr_code_size;
    int org_expr_big_endian = expr_big_endian;

    /* Initialize state */
    buf_pos = 0;
    buf_max = info->expr_size * 2;
    buf = (U1_T *)tmp_alloc(buf_max);
    expr_ctx = ctx;
    expr_frame = frame;
    expr_code_addr = 0;
    expr_code_size = 0;
    expr_big_endian = info->object->mCompUnit->mFile->big_endian;
    expr = NULL;
    jumps = NULL;

    /* Run transformation */
    if (elf_save_symbols_state(transform_expression, info) < 0) error = errno;
    memset(info, 0, sizeof(DWARFExpressionInfo));
    info->code_addr = expr_code_addr;
    info->code_size = expr_code_size;
    info->expr_addr = buf;
    info->expr_size = buf_pos;

    /* Restore state */
    buf = org_buf;
    buf_pos = org_buf_pos;
    buf_max = org_buf_max;
    jumps = org_jumps;
    expr = org_expr;
    expr_pos = org_expr_pos;
    expr_ctx = org_expr_ctx;
    expr_frame = org_expr_frame;
    expr_code_addr = org_expr_code_addr;
    expr_code_size = org_expr_code_size;
    expr_big_endian = org_expr_big_endian;

    if (error) exception(error);
}

#endif
