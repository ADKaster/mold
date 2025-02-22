#include "mold.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>

namespace mold::elf {

using E = ARM64;

static void write_adr(u8 *buf, u64 val) {
  u32 hi = (val & 0x1ffffc) << 3;
  u32 lo = (val & 3) << 29;
  *(u32 *)buf = (*(u32 *)buf & 0x9f00001f) | hi | lo;
}

// Returns [hi:lo] bits of val.
static u64 bits(u64 val, u64 hi, u64 lo) {
  return (val >> lo) & (((u64)1 << (hi - lo + 1)) - 1);
}

static u64 page(u64 val) {
  return val & ~(u64)0xfff;
}

template <>
void GotPltSection<E>::copy_buf(Context<E> &ctx) {
  u64 *buf = (u64 *)(ctx.buf + this->shdr.sh_offset);

  // The first slot of .got.plt points to _DYNAMIC.
  buf[0] = ctx.dynamic ? ctx.dynamic->shdr.sh_addr : 0;
  buf[1] = 0;
  buf[2] = 0;

  for (Symbol<E> *sym : ctx.plt->symbols)
    buf[sym->get_gotplt_idx(ctx)] = ctx.plt->shdr.sh_addr;
}

static void write_plt_header(Context<E> &ctx, u8 *buf) {
  // Write PLT header
  static const u8 plt0[] = {
    0xf0, 0x7b, 0xbf, 0xa9, // stp    x16, x30, [sp,#-16]!
    0x10, 0x00, 0x00, 0x90, // adrp   x16, .got.plt[2]
    0x11, 0x02, 0x40, 0xf9, // ldr    x17, [x16, .got.plt[2]]
    0x10, 0x02, 0x00, 0x91, // add    x16, x16, .got.plt[2]
    0x20, 0x02, 0x1f, 0xd6, // br     x17
    0x1f, 0x20, 0x03, 0xd5, // nop
    0x1f, 0x20, 0x03, 0xd5, // nop
    0x1f, 0x20, 0x03, 0xd5, // nop
  };

  u64 gotplt = ctx.gotplt->shdr.sh_addr + 16;
  u64 plt = ctx.plt->shdr.sh_addr;

  memcpy(buf, plt0, sizeof(plt0));
  write_adr(buf + 4, bits(page(gotplt) - page(plt + 4), 32, 12));
  *(u32 *)(buf + 8) |= bits(gotplt, 11, 3) << 10;
  *(u32 *)(buf + 12) |= ((gotplt) & 0xfff) << 10;
}

static void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  u8 *ent = buf + ctx.plt_hdr_size + sym.get_plt_idx(ctx) * ctx.plt_size;

  static const u8 data[] = {
    0x10, 0x00, 0x00, 0x90, // adrp x16, .got.plt[n]
    0x11, 0x02, 0x40, 0xf9, // ldr  x17, [x16, .got.plt[n]]
    0x10, 0x02, 0x00, 0x91, // add  x16, x16, .got.plt[n]
    0x20, 0x02, 0x1f, 0xd6, // br   x17
  };

  u64 gotplt = sym.get_gotplt_addr(ctx);
  u64 plt = sym.get_plt_addr(ctx);

  memcpy(ent, data, sizeof(data));
  write_adr(ent, bits(page(gotplt) - page(plt), 32, 12));
  *(u32 *)(ent + 4) |= bits(gotplt, 11, 3) << 10;
  *(u32 *)(ent + 8) |= (gotplt & 0xfff) << 10;
}

template <>
void PltSection<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + this->shdr.sh_offset;
  write_plt_header(ctx, buf);
  for (Symbol<E> *sym : symbols)
    write_plt_entry(ctx, buf, *sym);
}

template <>
void PltGotSection<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + this->shdr.sh_offset;

  for (Symbol<E> *sym : symbols) {
    u8 *ent = buf + sym->get_pltgot_idx(ctx) * ARM64::pltgot_size;

    static const u8 data[] = {
      0x10, 0x00, 0x00, 0x90, // adrp x16, GOT[n]
      0x11, 0x02, 0x40, 0xf9, // ldr  x17, [x16, GOT[n]]
      0x20, 0x02, 0x1f, 0xd6, // br   x17
      0x1f, 0x20, 0x03, 0xd5, // nop
    };

    u64 got = sym->get_got_addr(ctx);
    u64 plt = sym->get_plt_addr(ctx);

    memcpy(ent, data, sizeof(data));
    write_adr(ent, bits(page(got) - page(plt), 32, 12));
    *(u32 *)(ent + 4) |= bits(got, 11, 3) << 10;
  }
}

template <>
void EhFrameSection<E>::apply_reloc(Context<E> &ctx, ElfRel<E> &rel,
                                    u64 loc, u64 val) {
  u8 *base = ctx.buf + this->shdr.sh_offset;

  switch (rel.r_type) {
  case R_AARCH64_ABS64:
    *(u64 *)(base + loc) = val;
    return;
  case R_AARCH64_PREL32:
    *(u32 *)(base + loc) = val - this->shdr.sh_addr - loc;
    return;
  case R_AARCH64_PREL64:
    *(u64 *)(base + loc) = val - this->shdr.sh_addr - loc;
    return;
  }
  Fatal(ctx) << "unsupported relocation in .eh_frame: " << rel;
}

template <>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  ElfRel<E> *dynrel = nullptr;
  std::span<ElfRel<E>> rels = get_rels(ctx);

  i64 frag_idx = 0;

  if (ctx.reldyn)
    dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                           file.reldyn_offset + this->reldyn_offset);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_AARCH64_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    const SectionFragmentRef<E> *frag_ref = nullptr;
    if (rel_fragments && rel_fragments[frag_idx].idx == i)
      frag_ref = &rel_fragments[frag_idx++];

    auto overflow_check = [&](i64 val, i64 lo, i64 hi) {
      if (val < lo || hi <= val)
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " out of range: " << val << " is not in ["
                   << lo << ", " << hi << ")";
    };

#define S   (frag_ref ? frag_ref->frag->get_addr(ctx) : sym.get_addr(ctx))
#define A   (frag_ref ? frag_ref->addend : rel.r_addend)
#define P   (output_section->shdr.sh_addr + offset + rel.r_offset)
#define G   (sym.get_got_addr(ctx) - ctx.got->shdr.sh_addr)
#define GOT ctx.got->shdr.sh_addr

    if (needs_dynrel[i]) {
      *dynrel++ = {P, R_AARCH64_ABS64, (u32)sym.get_dynsym_idx(ctx), A};
      *(u64 *)loc = A;
      continue;
    }

    if (needs_baserel[i]) {
      if (!is_relr_reloc(ctx, rel))
        *dynrel++ = {P, R_AARCH64_RELATIVE, 0, (i64)(S + A)};
      *(u64 *)loc = S + A;
      continue;
    }

    switch (rel.r_type) {
    case R_AARCH64_ABS64:
      *(u64 *)loc = S + A;
      continue;
    case R_AARCH64_LDST8_ABS_LO12_NC:
      *(u32 *)loc |= bits(S + A, 11, 0) << 10;
      continue;
    case R_AARCH64_LDST16_ABS_LO12_NC:
      *(u32 *)loc |= bits(S + A, 11, 1) << 10;
      continue;
    case R_AARCH64_LDST32_ABS_LO12_NC:
      *(u32 *)loc |= bits(S + A, 11, 2) << 10;
      continue;
    case R_AARCH64_LDST64_ABS_LO12_NC:
      *(u32 *)loc |= bits(S + A, 11, 3) << 10;
      continue;
    case R_AARCH64_LDST128_ABS_LO12_NC:
      *(u32 *)loc |= bits(S + A, 11, 4) << 10;
      continue;
    case R_AARCH64_ADD_ABS_LO12_NC:
      *(u32 *)loc |= bits(S + A, 11, 0) << 10;
      continue;
    case R_AARCH64_MOVW_UABS_G0_NC:
      *(u32 *)loc |= bits(S + A, 15, 0) << 5;
      continue;
    case R_AARCH64_MOVW_UABS_G1_NC:
      *(u32 *)loc |= bits(S + A, 31, 16) << 5;
      continue;
    case R_AARCH64_MOVW_UABS_G2_NC:
      *(u32 *)loc |= bits(S + A, 47, 32) << 5;
      continue;
    case R_AARCH64_MOVW_UABS_G3:
      *(u32 *)loc |= bits(S + A, 63, 48) << 5;
      continue;
    case R_AARCH64_ADR_GOT_PAGE: {
      i64 val = page(G + GOT + A) - page(P);
      overflow_check(val, -((i64)1 << 32), (i64)1 << 32);
      write_adr(loc, bits(val, 32, 12));
      continue;
    }
    case R_AARCH64_ADR_PREL_PG_HI21: {
      i64 val = page(S + A) - page(P);
      overflow_check(val, -((i64)1 << 32), (i64)1 << 32);
      write_adr(loc, bits(val, 32, 12));
      continue;
    }
    case R_AARCH64_ADR_PREL_LO21: {
      i64 val = S + A - P;
      overflow_check(val, -((i64)1 << 20), (i64)1 << 20);
      write_adr(loc, val);
      continue;
    }
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26: {
      if (sym.esym().is_undef_weak()) {
        // On ARM, calling an weak undefined symbol jumps to the
        // next instruction.
        *(u32 *)loc |= 1;
        continue;
      }

      i64 lo = -(1 << 27);
      i64 hi = 1 << 27;
      i64 val = S + A - P;

      if (val < lo || hi <= val) {
        RangeExtensionRef ref = range_extn[i];
        val = output_section->thunks[ref.thunk_idx]->get_addr(ref.sym_idx) + A - P;
        assert(lo <= val && val < hi);
      }

      *(u32 *)loc |= (val >> 2) & 0x3ffffff;
      continue;
    }
    case R_AARCH64_CONDBR19: {
      i64 val = S + A - P;
      overflow_check(val, -((i64)1 << 20), (i64)1 << 20);
      *(u32 *)loc |= bits(val, 20, 2) << 5;
      continue;
    }
    case R_AARCH64_PREL16: {
      i64 val = S + A - P;
      overflow_check(val, -((i64)1 << 15), (i64)1 << 15);
      *(u16 *)loc = val;
      continue;
    }
    case R_AARCH64_PREL32: {
      i64 val = S + A - P;
      overflow_check(val, -((i64)1 << 31), (i64)1 << 32);
      *(u32 *)loc = val;
      continue;
    }
    case R_AARCH64_PREL64:
      *(u64 *)loc = S + A - P;
      continue;
    case R_AARCH64_LD64_GOT_LO12_NC:
      *(u32 *)loc |= bits(G + GOT + A, 11, 3) << 10;
      continue;
    case R_AARCH64_LD64_GOTPAGE_LO15: {
      i64 val = G + GOT + A - page(GOT);
      overflow_check(val, 0, 1 << 15);
      *(u32 *)loc |= bits(val, 14, 3) << 10;
      continue;
    }
    case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21: {
      i64 val = page(sym.get_gottp_addr(ctx) + A) - page(P);
      overflow_check(val, -((i64)1 << 32), (i64)1 << 32);
      write_adr(loc, bits(val, 32, 12));
      continue;
    }
    case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
      *(u32 *)loc |= bits(sym.get_gottp_addr(ctx) + A, 11, 3) << 10;
      continue;
    case R_AARCH64_TLSLE_ADD_TPREL_HI12: {
      i64 val = S + A - ctx.tls_begin + 16;
      overflow_check(val, 0, (i64)1 << 24);
      *(u32 *)loc |= bits(val, 23, 12) << 10;
      continue;
    }
    case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC:
      *(u32 *)loc |= bits(S + A - ctx.tls_begin + 16, 11, 0) << 10;
      continue;
    case R_AARCH64_TLSGD_ADR_PAGE21: {
      i64 val = page(sym.get_tlsgd_addr(ctx) + A) - page(P);
      overflow_check(val, -((i64)1 << 32), (i64)1 << 32);
      write_adr(loc, bits(val, 32, 12));
      continue;
    }
    case R_AARCH64_TLSGD_ADD_LO12_NC:
      *(u32 *)loc |= bits(sym.get_tlsgd_addr(ctx) + A, 11, 0) << 10;
      continue;
    case R_AARCH64_TLSDESC_ADR_PAGE21: {
      if (ctx.relax_tlsdesc && !sym.is_imported) {
        // adrp x0, 0 -> movz x0, #tls_ofset_hi, lsl #16
        i64 val = (S + A - ctx.tls_begin + 16);
        overflow_check(val, -((i64)1 << 32), (i64)1 << 32);
        *(u32 *)loc = 0xd2a00000 | (bits(val, 32, 16) << 5);
      } else {
        i64 val = page(sym.get_tlsdesc_addr(ctx) + A) - page(P);
        overflow_check(val, -((i64)1 << 32), (i64)1 << 32);
        write_adr(loc, bits(val, 32, 12));
      }
      continue;
    }
    case R_AARCH64_TLSDESC_LD64_LO12:
      if (ctx.relax_tlsdesc && !sym.is_imported) {
        // ldr x2, [x0] -> movk x0, #tls_ofset_lo
        u32 offset_lo = (S + A - ctx.tls_begin + 16) & 0xffff;
        *(u32 *)loc = 0xf2800000 | (offset_lo << 5);
      } else {
        *(u32 *)loc |= bits(sym.get_tlsdesc_addr(ctx) + A, 11, 3) << 10;
      }
      continue;
    case R_AARCH64_TLSDESC_ADD_LO12:
      if (ctx.relax_tlsdesc && !sym.is_imported) {
        // add x0, x0, #0 -> nop
        *(u32 *)loc = 0xd503201f;
      } else {
        *(u32 *)loc |= bits(sym.get_tlsdesc_addr(ctx) + A, 11, 0) << 10;
      }
      continue;
    case R_AARCH64_TLSDESC_CALL:
      if (ctx.relax_tlsdesc && !sym.is_imported) {
        // blr x2 -> nop
        *(u32 *)loc = 0xd503201f;
      }
      continue;
    default:
      unreachable();
    }

#undef S
#undef A
#undef P
#undef G
#undef GOT
  }
}

template <>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<ElfRel<E>> rels = get_rels(ctx);
  i64 frag_idx = 0;

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_AARCH64_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    if (!sym.file) {
      report_undef(ctx, sym);
      continue;
    }

    SectionFragment<E> *frag;
    i64 addend;
    std::tie(frag, addend) = get_fragment(ctx, rel);

#define S (frag ? frag->get_addr(ctx) : sym.get_addr(ctx))
#define A (frag ? addend : rel.r_addend)

    switch (rel.r_type) {
    case R_AARCH64_ABS64:
      *(u64 *)loc = S + A;
      continue;
    case R_AARCH64_ABS32:
      *(u32 *)loc = S + A;
      continue;
    default:
      Fatal(ctx) << *this << ": invalid relocation for non-allocated sections: "
                 << rel;
      break;
    }

#undef S
#undef A
  }
}

template <>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr.sh_flags & SHF_ALLOC);

  this->reldyn_offset = file.num_dynrel * sizeof(ElfRel<E>);
  std::span<ElfRel<E>> rels = get_rels(ctx);

  // Scan relocations
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_AARCH64_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    if (!sym.file) {
      report_undef(ctx, sym);
      continue;
    }

    if (sym.get_type() == STT_GNU_IFUNC) {
      sym.flags |= NEEDS_GOT;
      sym.flags |= NEEDS_PLT;
    }

    switch (rel.r_type) {
    case R_AARCH64_ABS64: {
      Action table[][4] = {
        // Absolute  Local    Imported data  Imported code
        {  NONE,     BASEREL, DYNREL,        DYNREL },     // DSO
        {  NONE,     BASEREL, DYNREL,        DYNREL },     // PIE
        {  NONE,     NONE,    COPYREL,       PLT    },     // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_AARCH64_ADR_GOT_PAGE:
    case R_AARCH64_LD64_GOT_LO12_NC:
    case R_AARCH64_LD64_GOTPAGE_LO15:
      sym.flags |= NEEDS_GOT;
      break;
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26:
      if (sym.is_imported)
        sym.flags |= NEEDS_PLT;
      break;
    case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
    case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
      sym.flags |= NEEDS_GOTTP;
      break;
    case R_AARCH64_ADR_PREL_PG_HI21: {
      Action table[][4] = {
        // Absolute  Local    Imported data  Imported code
        {  NONE,     NONE,    ERROR,         ERROR },      // DSO
        {  NONE,     NONE,    COPYREL,       PLT   },      // PIE
        {  NONE,     NONE,    COPYREL,       PLT   },      // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_AARCH64_TLSGD_ADR_PAGE21:
      sym.flags |= NEEDS_TLSGD;
      break;
    case R_AARCH64_TLSDESC_ADR_PAGE21:
    case R_AARCH64_TLSDESC_LD64_LO12:
    case R_AARCH64_TLSDESC_ADD_LO12:
      if (!ctx.relax_tlsdesc || sym.is_imported)
        sym.flags |= NEEDS_TLSDESC;
      break;
    case R_AARCH64_ADD_ABS_LO12_NC:
    case R_AARCH64_ADR_PREL_LO21:
    case R_AARCH64_CONDBR19:
    case R_AARCH64_LDST16_ABS_LO12_NC:
    case R_AARCH64_LDST32_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:
    case R_AARCH64_LDST128_ABS_LO12_NC:
    case R_AARCH64_LDST8_ABS_LO12_NC:
    case R_AARCH64_MOVW_UABS_G0_NC:
    case R_AARCH64_MOVW_UABS_G1_NC:
    case R_AARCH64_MOVW_UABS_G2_NC:
    case R_AARCH64_MOVW_UABS_G3:
    case R_AARCH64_PREL16:
    case R_AARCH64_PREL32:
    case R_AARCH64_PREL64:
    case R_AARCH64_TLSLE_ADD_TPREL_HI12:
    case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC:
    case R_AARCH64_TLSGD_ADD_LO12_NC:
    case R_AARCH64_TLSDESC_CALL:
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

static void reset_thunk(RangeExtensionThunk<E> &thunk) {
  for (Symbol<E> *sym : thunk.symbols) {
    sym->thunk_idx = -1;
    sym->thunk_sym_idx = -1;
    sym->flags &= (u8)~NEEDS_THUNK;
  }
}

static bool is_reachable(Context<E> &ctx, Symbol<E> &sym,
                         InputSection<E> &isec, const ElfRel<E> &rel) {
  // We always create a thunk for an absolute symbol conservatively
  // because `shrink_sections` may increase a distance between a
  // branch instruction and an absolute symbol. Branching to an
  // absolute location is extremely rare in real code, though.
  if (sym.is_absolute())
    return false;

  // Compute a distance between the relocated place and the symbol
  // and check if they are within reach.
  i64 S = sym.get_addr(ctx);
  i64 A = rel.r_addend;
  i64 P = isec.get_addr() + rel.r_offset;
  i64 val = S + A - P;
  return -(1 << 27) <= val && val < (1 << 27);
}

// We create a thunk no further than 100 MiB from any section.
constexpr i64 MAX_DISTANCE = 100 * 1024 * 1024;

// We create a thunk for each 10 MiB input sections.
static constexpr i64 GROUP_SIZE = 10 * 1024 * 1024;

static void create_thunks(Context<E> &ctx, OutputSection<E> &osec) {
  std::span<InputSection<E> *> members = osec.members;
  members[0]->offset = 0;

  // Initialize input sections with very large dummy offsets so that
  // sections that have got real offsets are separated from the ones
  // without in the virtual address space.
  tbb::parallel_for((i64)1, (i64)members.size(), [&](i64 i) {
    members[i]->offset = 1 << 31;
  });

  // We create thunks from the beginning of the section to the end.
  // We manage progress using four offsets which increase monotonically.
  // The locations they point to are always A <= B <= C <= D.
  i64 a = 0;
  i64 b = 0;
  i64 c = 0;
  i64 d = 0;
  i64 offset = 0;

  while (b < members.size()) {
    // Move D foward as far as we can jump from B to D.
    while (d < members.size() && offset - members[b]->offset < MAX_DISTANCE) {
      offset = align_to(offset, members[d]->shdr.sh_addralign);
      members[d]->offset = offset;
      offset += members[d]->shdr.sh_size;
      d++;
    }

    // Move C forward so that C is apart from B by GROUP_SIZE.
    while (c < members.size() &&
           members[c]->offset - members[b]->offset < GROUP_SIZE)
      c++;

    // Move A forward so that A is reachable from C.
    if (c > 0) {
      i64 c_end = members[c - 1]->offset + members[c - 1]->shdr.sh_size;
      while (a < osec.thunks.size() &&
             osec.thunks[a]->offset < c_end - MAX_DISTANCE)
        reset_thunk(*osec.thunks[a++]);
    }

    // Create a thunk for input sections between B and C and place it at D.
    osec.thunks.emplace_back(new RangeExtensionThunk<E>{osec});

    RangeExtensionThunk<E> &thunk = *osec.thunks.back();
    thunk.thunk_idx = osec.thunks.size() - 1;
    thunk.offset = offset;

    // Scan relocations between B and C to collect symbols that need thunks.
    tbb::parallel_for_each(&members[b], &members[c], [&](InputSection<E> *isec) {
      std::span<ElfRel<E>> rels = isec->get_rels(ctx);
      isec->range_extn.resize(rels.size());

      for (i64 i = 0; i < rels.size(); i++) {
        const ElfRel<E> &rel = rels[i];
        if (rel.r_type != R_AARCH64_CALL26 && rel.r_type != R_AARCH64_JUMP26)
          continue;

        Symbol<E> &sym = *isec->file.symbols[rel.r_sym];

        // Skip if the destination is within reach.
        if (is_reachable(ctx, sym, *isec, rel))
          continue;

        // If the symbol is already in another thunk, reuse it.
        if (sym.thunk_idx != -1) {
          isec->range_extn[i] = {sym.thunk_idx, sym.thunk_sym_idx};
          continue;
        }

        // Otherwise, add the symbol to this thunk if it's not added already.
        isec->range_extn[i] = {thunk.thunk_idx, -1};

        if (!(sym.flags.fetch_or(NEEDS_THUNK) & NEEDS_THUNK)) {
          std::scoped_lock lock(thunk.mu);
          thunk.symbols.push_back(&sym);
        }
      }
    });

    // Now that we know the number of symbols in the thunk, we can compute
    // its size.
    offset += thunk.size();

    // Sort symbols added to the thunk to make the output deterministic.
    sort(thunk.symbols, [](Symbol<E> *a, Symbol<E> *b) { return *a < *b; });

    // Assign offsets within the thunk to the symbols.
    for (i64 i = 0; Symbol<E> *sym : thunk.symbols) {
      sym->thunk_idx = thunk.thunk_idx;
      sym->thunk_sym_idx = i++;
    }

    // Scan relocations again to fix symbol offsets in the last thunk.
    tbb::parallel_for_each(&members[b], &members[c], [&](InputSection<E> *isec) {
      std::span<ElfRel<E>> rels = isec->get_rels(ctx);

      for (i64 i = 0; i < rels.size(); i++) {
        RangeExtensionRef &ref = isec->range_extn[i];

        if (ref.thunk_idx == thunk.thunk_idx) {
          Symbol<E> &sym = *isec->file.symbols[rels[i].r_sym];
          ref.sym_idx = sym.thunk_sym_idx;
        }
      }
    });

    // Move B forward to point to the begining of the next group.
    b = c;
  }

  while (a < osec.thunks.size())
    reset_thunk(*osec.thunks[a++]);

  osec.shdr.sh_size = offset;
}

static void mark_thunk_symbols(Context<E> &ctx, OutputSection<E> &osec) {
  for (std::unique_ptr<RangeExtensionThunk<E>> &thunk : osec.thunks) {
    i64 sz = thunk->symbols.size();
    thunk->symbol_map.resize(sz);
    thunk->used.reset(new std::atomic_bool[sz]{});
  }

  // Mark referenced thunk symbols
  tbb::parallel_for_each(osec.members, [&](InputSection<E> *isec) {
    std::span<ElfRel<E>> rels = isec->get_rels(ctx);

    for (i64 i = 0; i < rels.size(); i++) {
      RangeExtensionRef &ref = isec->range_extn[i];
      if (ref.thunk_idx == -1)
        continue;

      Symbol<E> &sym = *isec->file.symbols[rels[i].r_sym];
      if (!is_reachable(ctx, sym, *isec, rels[i]))
        osec.thunks[ref.thunk_idx]->used[ref.sym_idx] = true;
    }
  });

  // Remove unreferenced thunk symbols
  tbb::parallel_for_each(osec.thunks,
                         [&](std::unique_ptr<RangeExtensionThunk<E>> &thunk) {
    i64 i = 0;
    for (i64 j = 0; j < thunk->symbols.size(); j++) {
      if (thunk->used[j]) {
        thunk->symbol_map[j] = i;
        thunk->symbols[i] = thunk->symbols[j];
        i++;
      }
    }
    thunk->symbols.resize(i);
  });
}

static void shrink_section(Context<E> &ctx, OutputSection<E> &osec) {
  std::span<std::unique_ptr<RangeExtensionThunk<E>>> thunks = osec.thunks;
  std::span<InputSection<E> *> members = osec.members;

  i64 offset = 0;

  auto add_thunk = [&]() {
    thunks[0]->offset = offset;
    offset += thunks[0]->size();
    thunks = thunks.subspan(1);
  };

  auto add_isec = [&]() {
    offset = align_to(offset, members[0]->shdr.sh_addralign);
    members[0]->offset = offset;
    offset += members[0]->shdr.sh_size;
    members = members.subspan(1);
  };

  while (!thunks.empty() && !members.empty()) {
    if (thunks[0]->offset < members[0]->offset)
      add_thunk();
    else
      add_isec();
  }

  while (!thunks.empty())
    add_thunk();
  while (!members.empty())
    add_isec();

  assert(offset <= osec.shdr.sh_size);
  osec.shdr.sh_size = offset;
}

// ARM64's call/jump instructions take 27 bits displacement, so they
// can refer only up to ±128 MiB. If a branch target is further than
// that, we need to let it branch to a linker-synthesized code
// sequence that construct a full 32 bit address in a register and
// jump there. That linker-synthesized code is called "thunk".
i64 create_range_extension_thunks(Context<E> &ctx) {
  Timer t(ctx, "create_range_extension_thunks");

  // First, we create thunks with a pessimistic assumption that all
  // out-of-section relocations would need thunks. To do so, we start
  // with an initial layout in which output sections are separated far
  // apart.
  for (i64 i = 0; Chunk<E> *chunk : ctx.chunks)
    chunk->shdr.sh_addr = i++ << 31;

  std::vector<OutputSection<E> *> sections;
  for (std::unique_ptr<OutputSection<E>> &osec : ctx.output_sections)
    if (!osec->members.empty() && (osec->shdr.sh_flags & SHF_EXECINSTR))
      sections.push_back(osec.get());

  for (OutputSection<E> *osec : sections)
    create_thunks(ctx, *osec);

  // Recompute file layout.
  set_osec_offsets(ctx);

  // Based on the current file layout, remove thunk symbols that turned
  // to be unnecessary.
  tbb::parallel_for_each(sections, [&](OutputSection<E> *osec) {
    mark_thunk_symbols(ctx, *osec);
  });

  // Recompute output section sizes that contain thunks. New section
  // sizes must be equal to or smaller than previous values, so all
  // relocations that were previously reachable will still be reachable
  // after this step.
  for (OutputSection<E> *osec : sections)
    shrink_section(ctx, *osec);

  // Compute the final layout.
  return set_osec_offsets(ctx);
}

void RangeExtensionThunk<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + output_section.shdr.sh_offset + offset;

  static const u8 insn[] = {
    0x10, 0x00, 0x00, 0x90, // adrp x16, 0   # R_AARCH64_ADR_PREL_PG_HI21
    0x10, 0x02, 0x00, 0x91, // add  x16, x16 # R_AARCH64_ADD_ABS_LO12_NC
    0x00, 0x02, 0x1f, 0xd6, // br   x16
  };

  static_assert(ENTRY_SIZE == sizeof(insn));

  for (i64 i = 0; i < symbols.size(); i++) {
    u64 S = symbols[i]->get_addr(ctx);
    u64 P = output_section.shdr.sh_addr + offset + i * ENTRY_SIZE;

    u8 *loc = buf + i * ENTRY_SIZE;
    memcpy(loc , insn, sizeof(insn));
    write_adr(loc, bits(page(S) - page(P), 32, 12));
    *(u32 *)(loc + 4) |= bits(S, 11, 0) << 10;
  }
}

void write_thunks(Context<E> &ctx) {
  Timer t(ctx, "write_thunks");

  tbb::parallel_for_each(ctx.output_sections,
                         [&](std::unique_ptr<OutputSection<E>> &osec) {
    tbb::parallel_for_each(osec->thunks,
                           [&](std::unique_ptr<RangeExtensionThunk<E>> &thunk) {
      thunk->copy_buf(ctx);
    });
  });
}

} // namespace mold::elf
