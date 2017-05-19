/* output_hunk.c AmigaOS hunk format output driver for vasm */
/* (c) in 2002-2017 by Frank Wille */

#include "vasm.h"
#include "output_hunk.h"
#if defined(OUTHUNK) && (defined(VASM_CPU_M68K) || defined(VASM_CPU_PPC))
static char *copyright="vasm hunk format output module 2.9a (c) 2002-2017 Frank Wille";
int hunk_onlyglobal;

static int databss;
static int kick1;
static int exthunk;
static int genlinedebug;
static int keep_empty_sects;

static uint32_t sec_cnt;
static symbol **secsyms;


static uint32_t strlen32(char *s)
/* calculate number of 32 bit words required for
   a string without terminator */
{
  return (strlen(s) + 3) >> 2;
}


static void fwname(FILE *f,char *name)
{
  size_t n = strlen(name);

  fwdata(f,name,n);
  fwalign(f,n,4);
}


static void fwmemflags(FILE *f,section *s,uint32_t data)
/* write memory attributes with data (section size or type) */
{
  uint32_t mem = s->memattr;

  if ((mem & ~MEMF_PUBLIC) == 0) {
    fw32(f,data,1);
  }
  else if ((mem & ~MEMF_PUBLIC) == MEMF_CHIP) {
    fw32(f,HUNKF_CHIP|data,1);
  }
  else if ((mem & ~MEMF_PUBLIC) == MEMF_FAST) {
    fw32(f,HUNKF_FAST|data,1);
  }
  else {
    fw32(f,HUNKF_MEMTYPE|data,1);
    fw32(f,mem,1);
  }
}


static void fwnopalign(FILE *f,taddr n)
/* align a 68k code section with NOP instructions */
{
  taddr i;

  n = balign(n,4);
  if (n & 1)
    ierror(0);
  for (i=0; i<n; i+=2)
    fw16(f,0x4e71,1);
}


static section *dummy_section(void)
{
  return new_section(".text","acrx3",8);
}


static uint32_t scan_attr(section *sec)
/* extract hunk-type from section attributes */
{
  uint32_t type = 0;
  char *p = sec->attr;

  if (*p != '\0') {
    while (*p) {
      switch (*p++) {
#if defined(VASM_CPU_PPC)
        case 'c': type = HUNK_PPC_CODE; break;
#else
        case 'c': type = HUNK_CODE; break;
#endif
        case 'd': type = HUNK_DATA; break;
        case 'u': type = HUNK_BSS; break;
      }
    }
  }
  else
    type = HUNK_DATA;

  return type;
}


static section *prepare_sections(section *first_sec,symbol *sym)
/* Remove empty sections.
   Assign all symbol definitions to their respective section.
   Make sure that every common-symbol is referenced, otherwise there
   is no possibility to represent such a symbol in hunk format.
   Additionally we have to guarantee that at least one section exists,
   when there are any symbols.
   Remember the total number of sections left in sec_cnt. */
{
  section *sec,*first_nonbss;
  symbol *nextsym;
  rlist *rl;
  atom *a;

  for (sec_cnt=0,sec=first_sec,first_nonbss=NULL; sec!=NULL; sec=sec->next) {
    /* ignore empty sections without symbols, unless -keepempty was given */
    if (keep_empty_sects || get_sec_size(sec)!=0 || (sec->flags&HAS_SYMBOLS)) {
      sec->idx = sec_cnt++;
    }
    else {
      sec->flags |= SEC_DELETED;
      continue;
    }

    /* determine first initialized section for common-symbol references */
    if (first_nonbss==NULL && scan_attr(sec)!=HUNK_BSS)
      first_nonbss = sec;

    /* flag all present common-symbol references from this section */
    for (a=sec->first; a; a=a->next) {
      if (a->type == DATA) {
        for (rl=a->content.db->relocs; rl; rl=rl->next) {
          if (rl->type==REL_ABS || rl->type==REL_PC) {
            if (((nreloc *)rl->reloc)->size==32) {
              symbol *s = ((nreloc *)rl->reloc)->sym;

              if (s->flags & COMMON)
                s->flags |= COMM_REFERENCED;
            }
          }
        }
      }
    }
  }

  /* Allocate symbol lists for all sections.
     Get one more for a potential dummy section. */
  secsyms = mycalloc((sec_cnt+1)*sizeof(symbol *));

  /* Assign all valid symbol definitions to their section symbol list.
     Check for missing common-symbol references. */
  while (sym != NULL) {
    nextsym = sym->next;

    if (*sym->name != ' ') {  /* internal symbols will be ignored */
      if ((sym->flags & COMMON) && !(sym->flags & COMM_REFERENCED)) {
        /* create a dummy reference for each unreferenced common symbol */
        dblock *db = new_dblock();
        nreloc *r = new_nreloc();
        rlist *rl = mymalloc(sizeof(rlist));

        db->size = 4;
        db->data = mycalloc(db->size);
        db->relocs = rl;
        rl->next = NULL;
        rl->type = REL_ABS;
        rl->reloc = r;
        r->size = 32;
        r->sym = sym;
        if (first_nonbss == NULL) {
          first_nonbss = dummy_section();
          if (first_sec == NULL)
            first_sec = first_nonbss;
          first_nonbss->idx = sec_cnt++;
        }
        add_atom(first_nonbss,new_data_atom(db,4));
      }
      else if (sym->flags & WEAK) {
        /* weak symbols are not supported, make them global */
        sym->flags &= ~WEAK;
        sym->flags |= EXPORT;
        output_error(10,sym->name);
      }

      if (sym->type==LABSYM ||
               (sym->type==EXPRESSION && (sym->flags&EXPORT))) {
        if (sym->type == EXPRESSION) {
          /* put absolute globals symbols into the first section */
          if (first_sec == NULL) {
            first_sec = first_nonbss = dummy_section();
            first_sec->idx = sec_cnt++;
          }
          first_sec->flags |= HAS_SYMBOLS;
          sym->sec = first_sec;
        }
        /* assign symbols to the section they are defined in */
        sym->next = secsyms[sym->sec->idx];
        secsyms[sym->sec->idx] = sym;
      }
    }

    sym = nextsym;
  }
  return first_sec;
}


static utaddr file_size(section *sec)
/* determine a section's initialized data size, which occupies space in
   the executable file */
{
  utaddr pc=0,zpc=0,npc;
  atom *a;

  for (a=sec->first; a; a=a->next) {
    int zerodata = 1;
    unsigned char *d;

    npc = pcalign(a,pc);
    if (a->type == DATA) {
      /* do we have relocations or non-zero data in this atom? */
      if (a->content.db->relocs) {
        zerodata = 0;
      }
      else {
       	for (d=a->content.db->data;
             d<a->content.db->data+a->content.db->size; d++) {
          if (*d) {
            zerodata = 0;
            break;
          }
        }
      }
    }
    else if (a->type == SPACE) {
      /* do we have relocations or non-zero data in this atom? */
      if (a->content.sb->relocs) {
        zerodata = 0;
      }
      else {
        for (d=a->content.sb->fill;
             d<a->content.sb->fill+a->content.sb->size; d++) {
          if (*d) {
            zerodata = 0;
            break;
          }
        }
      }
    }
    pc = npc + atom_size(a,sec,npc);
    if (!zerodata)
      zpc = pc;
  }
  return zpc;
}


static struct hunkreloc *convert_reloc(rlist *rl,utaddr pc)
{
  nreloc *r = (nreloc *)rl->reloc;

  if (rl->type <= LAST_STANDARD_RELOC
#if defined(VASM_CPU_PPC)
      || rl->type==REL_PPCEABI_SDA2
#endif
     ) {
    if (LOCREF(r->sym)) {
      struct hunkreloc *hr;
      uint32_t type;
      uint32_t offs = pc + r->byteoffset;

      switch (rl->type) {
        case REL_ABS:
          if (r->size!=32 || r->bitoffset!=0 || r->mask!=-1)
            return NULL;
          type = HUNK_ABSRELOC32;
          break;

        case REL_PC:
          switch (r->size) {
            case 8:
              if (r->bitoffset!=0 || r->mask!=-1)
                return NULL;
              type = HUNK_RELRELOC8;
              break;
#if defined(VASM_CPU_PPC)
            case 14:
              if (r->bitoffset!=0 || r->mask!=0xfffc)
                return NULL;
              type = HUNK_RELRELOC16;
              break;
#endif
            case 16:
              if (r->bitoffset!=0 || r->mask!=-1)
                return NULL;
              type = HUNK_RELRELOC16;
              break;
#if defined(VASM_CPU_PPC)
            case 24:
              if (r->bitoffset!=6 || r->mask!=0x3fffffc)
                return NULL;
              type = HUNK_RELRELOC26;
              break;
#endif
            case 32:
              if (kick1 || r->bitoffset!=0 || r->mask!=-1)
                return NULL;
              type = HUNK_RELRELOC32;
              break;
          }
          break;

#if defined(VASM_CPU_PPC)
        case REL_PPCEABI_SDA2: /* treat as REL_SD for WarpOS/EHF */
#endif
        case REL_SD:
          if (r->size!=16 || r->bitoffset!=0 || r->mask!=-1)
            return NULL;
          type = HUNK_DREL16;
          break;

        default:
          return NULL;
      }

      hr = mymalloc(sizeof(struct hunkreloc));
      hr->hunk_id = type;
      hr->hunk_offset = offs;
      hr->hunk_index = r->sym->sec->idx;
      return hr;
    }
  }

  return NULL;
}


static struct hunkxref *convert_xref(rlist *rl,utaddr pc)
{
  nreloc *r = (nreloc *)rl->reloc;

  if (rl->type <= LAST_STANDARD_RELOC
#if defined(VASM_CPU_PPC)
      || rl->type==REL_PPCEABI_SDA2
#endif
     ) {
    if (EXTREF(r->sym)) {
      struct hunkxref *xref;
      uint32_t type,size=0;
      uint32_t offs = pc + r->byteoffset;
      int com = (r->sym->flags & COMMON) != 0;

      switch (rl->type) {
        case REL_ABS:
          if (r->bitoffset!=0 || r->mask!=-1 || (com && r->size!=32))
            return NULL;
          switch (r->size) {
            case 8:
              type = kick1 ? EXT_RELREF8 : EXT_ABSREF8;
              break;
            case 16:
              type = kick1 ? EXT_RELREF16 : EXT_ABSREF16;
              break;
            case 32:
              if (com) {
                type = EXT_ABSCOMMON;
                size = get_sym_size(r->sym);
              }
              else
                type = EXT_ABSREF32;
              break;
          }
          break;

        case REL_PC:
          switch (r->size) {
            case 8:
              if (r->bitoffset!=0 || r->mask!=-1 || com)
                return NULL;
              type = EXT_RELREF8;
              break;
#if defined(VASM_CPU_PPC)
            case 14:
              if (r->bitoffset!=0 || r->mask!=0xfffc || com)
                return NULL;
              type = EXT_RELREF16;
              break;
#endif
            case 16:
              if (r->bitoffset!=0 || r->mask!=-1 || com)
                return NULL;
              type = EXT_RELREF16;
              break;
#if defined(VASM_CPU_PPC)
            case 24:
              if (r->bitoffset!=6 || r->mask!=0x3fffffc || com)
                return NULL;
              type = EXT_RELREF26;
              break;
#endif
            case 32:
              if (kick1 || r->bitoffset!=0 || r->mask!=-1)
                return NULL;
              if (com) {
                type = EXT_RELCOMMON;
                size = get_sym_size(r->sym);
              }
              else
                type = EXT_RELREF32;
              break;
          }
          break;

#if defined(VASM_CPU_PPC)
        case REL_PPCEABI_SDA2: /* treat as REL_SD for WarpOS/EHF */
#endif
        case REL_SD:
          if (r->size!=16 || r->bitoffset!=0 || r->mask!=-1)
            return NULL;
          type = EXT_DEXT16;
          break;

        default:
          return NULL;
      }

      xref = mymalloc(sizeof(struct hunkxref));
      xref->name = r->sym->name;
      xref->type = type;
      xref->size = size;
      xref->offset = offs;
      return xref;
    }
  }

  return NULL;
}


static void process_relocs(rlist *rl,struct list *reloclist,
                           struct list *xreflist,section *sec,utaddr pc)
/* convert an atom's rlist into relocations and xrefs */
{
  if (rl == NULL)
    return;

  do {
    struct hunkreloc *hr = convert_reloc(rl,pc);

    if (hr!=NULL && (xreflist!=NULL || hr->hunk_id==HUNK_ABSRELOC32 ||
                     hr->hunk_id==HUNK_RELRELOC32)) {
      addtail(reloclist,&hr->n);       /* add new relocation */
    }
    else {
      struct hunkxref *xref = convert_xref(rl,pc);

      if (xref) {
        if (xreflist)
          addtail(xreflist,&xref->n);  /* add new external reference */
        else
          output_error(8,xref->name,sec->name,xref->offset,rl->type);
      }
      else
        unsupp_reloc_error(rl);  /* reloc not supported */
    }
  }
  while (rl = rl->next);
}


static void reloc_hunk(FILE *f,uint32_t type,int shrt,struct list *reloclist)
/* write all section-offsets for one relocation type */
{
  int bytes = 0;
  uint32_t idx;

  for (idx=0; idx<sec_cnt; idx++) {
    struct hunkreloc *r,*next;
    uint32_t n;
    int off16;

    for (r=(struct hunkreloc *)reloclist->first,n=0,off16=1;
         r->n.next; r=(struct hunkreloc *)r->n.next) {
      if (r->hunk_id==type && r->hunk_index==idx) {
        n++;
        if (r->hunk_offset >= 0x10000)
          off16 = 0;
      }
    }
    if (shrt && (n>=0x10000 || off16==0))
      continue;  /* relocs for this hunk don't fit into 16-bit entries */
    if (n > 0) {
      if (bytes == 0) {
        if (shrt && type==HUNK_ABSRELOC32)
          fw32(f,HUNK_DREL32,1);  /* RELOC32SHORT is DREL32 for OS2.0 */
        else
          fw32(f,type,1);
        bytes = 4;
      }
      if (shrt) {
        fw16(f,n,1);
        fw16(f,idx,1);
        bytes += 4;
      }
      else {
        fw32(f,n,1);
        fw32(f,idx,1);
        bytes += 8;
      }
      r = (struct hunkreloc *)reloclist->first;
      while (next = (struct hunkreloc *)r->n.next) {
        if (r->hunk_id==type && r->hunk_index==idx) {
          if (shrt) {
            fw16(f,r->hunk_offset,1);
            bytes += 2;
          }
          else {
            fw32(f,r->hunk_offset,1);
            bytes += 4;
          }
          remnode(&r->n);
          myfree(r);
        }
        r = next;
      }
    }
  }
  if (bytes) {
    if (shrt) {
      fw16(f,0,1);
      fwalign(f,bytes+2,4);
    }
    else
      fw32(f,0,1);
  }
}


static void add_linedebug(struct list *ldblist,uint32_t line,uint32_t off)
{
  struct hunkline *ldebug = mymalloc(sizeof(struct hunkline));

  ldebug->line = line;
  ldebug->offset = off;
  addtail(ldblist,&ldebug->n);
}


static void linedebug_hunk(FILE *f,struct list *ldblist,int num)
{
  if (num > 0) {
    struct hunkline *hl;
    uint32_t srcname_len = strlen32(getdebugname());

    fw32(f,HUNK_DEBUG,1);
    fw32(f,srcname_len + num*2 + 3,1);
    fw32(f,0,1);
    fw32(f,0x4c494e45,1);  /* "LINE" */
    fw32(f,srcname_len,1);
    fwname(f,getdebugname());

    for (hl=(struct hunkline *)ldblist->first;
         hl->n.next; hl=(struct hunkline *)hl->n.next) {
      fw32(f,hl->line,1);
      fw32(f,hl->offset,1);
    }
  }
}


static void extheader(FILE *f)
{
  if (!exthunk) {
    exthunk = 1;
    fw32(f,HUNK_EXT,1);
  }
}


static void exttrailer(FILE *f)
{
  if (exthunk)
    fw32(f,0,1);
}


static void ext_refs(FILE *f,struct list *xreflist)
/* write all external references from a section into a HUNK_EXT hunk */
{
  while (xreflist->first->next) {
    struct hunkxref *x,*next;
    uint32_t n,type,size;
    char *name;

    extheader(f);
    x = (struct hunkxref *)xreflist->first;
    name = x->name;
    type = x->type;
    size = x->size;

    for (n=0,x=(struct hunkxref *)xreflist->first;
         x->n.next; x=(struct hunkxref *)x->n.next) {
      if (!strcmp(x->name,name) && x->type==type)
        n++;
    }
    fw32(f,(type<<24) | strlen32(name),1);
    fwname(f,name);
    if (type==EXT_ABSCOMMON || type==EXT_RELCOMMON)
      fw32(f,size,1);
    fw32(f,n,1);

    x = (struct hunkxref *)xreflist->first;
    while (next = (struct hunkxref *)x->n.next) {
      if (!strcmp(x->name,name) && x->type==type) {
        fw32(f,x->offset,1);
        remnode(&x->n);
        myfree(x);
      }
      x = next;
    }
  }
}


static void ext_defs(FILE *f,int symtype,int global,size_t idx,
                     uint32_t xtype)
{
  int header = 0;
  symbol *sym;

  for (sym=secsyms[idx]; sym; sym=sym->next) {
    if (sym->type==symtype && (sym->flags&global)==global) {
      if (!header) {
        header = 1;
        if (xtype == EXT_SYMB)
          fw32(f,HUNK_SYMBOL,1);
        else
          extheader(f);
      }
      fw32(f,(xtype<<24) | strlen32(sym->name),1);
      fwname(f,sym->name);
      fw32(f,(uint32_t)get_sym_value(sym),1);
    }
  }
  if (header && xtype==EXT_SYMB)
    fw32(f,0,1);
}


static void write_object(FILE *f,section *sec,symbol *sym)
{
  int wrotesec = 0;

  sec = prepare_sections(sec,sym);

  /* write header */
  fw32(f,HUNK_UNIT,1);
  fw32(f,strlen32(filename),1);
  fwname(f,filename);

  if (sec) {
    for (; sec; sec=sec->next) {
      if (!(sec->flags & SEC_DELETED)) {
        uint32_t type;
        atom *a;
        struct list reloclist,xreflist,linedblist;
        int num_linedb = 0;

        wrotesec = 1;
        initlist(&reloclist);
        initlist(&xreflist);
        initlist(&linedblist);

        /* section name */
        if (strlen(sec->name)) {
          fw32(f,HUNK_NAME,1);
          fw32(f,strlen32(sec->name),1);
          fwname(f,sec->name);
        }

        /* section type */
        if (!(type = scan_attr(sec))) {
          output_error(3,sec->attr);  /* section attributes not suppported */
          type = HUNK_DATA;  /* default */
        }
        fwmemflags(f,sec,type);
        fw32(f,(get_sec_size(sec)+3)>>2,1);  /* size */

        if (type != HUNK_BSS) {
          /* write contents */
          utaddr pc=0,npc,i;

          for (a=sec->first; a; a=a->next) {
            rlist *rl;

            npc = fwpcalign(f,a,sec,pc);

            if (genlinedebug && (a->type==DATA || a->type==SPACE)) {
              add_linedebug(&linedblist,(uint32_t)a->line,npc);
              ++num_linedb;
            }
            if (a->type == DATA) {
              fwdata(f,a->content.db->data,a->content.db->size);
              process_relocs(a->content.db->relocs,
                             &reloclist,&xreflist,sec,npc);
            }
            else if (a->type == SPACE) {
              fwsblock(f,a->content.sb);
              process_relocs(a->content.sb->relocs,
                             &reloclist,&xreflist,sec,npc);
            }
            else if (a->type == LINE && !genlinedebug) {
              add_linedebug(&linedblist,(uint32_t)a->content.srcline,npc);
              ++num_linedb;
            }

            pc = npc + atom_size(a,sec,npc);
          }
          if (type == HUNK_CODE && (pc&1) == 0)
            fwnopalign(f,pc);
          else
            fwalign(f,pc,4);
        }

        /* relocation hunks */
        reloc_hunk(f,HUNK_ABSRELOC32,0,&reloclist);
        reloc_hunk(f,HUNK_RELRELOC8,0,&reloclist);
        reloc_hunk(f,HUNK_RELRELOC16,0,&reloclist);
        reloc_hunk(f,HUNK_RELRELOC26,0,&reloclist);
        reloc_hunk(f,HUNK_RELRELOC32,0,&reloclist);
        reloc_hunk(f,HUNK_DREL16,0,&reloclist);

        /* external references and global definitions */
        exthunk = 0;
        ext_refs(f,&xreflist);
        if (sec->idx == 0)  /* absolute definitions into first hunk */
          ext_defs(f,EXPRESSION,EXPORT,0,EXT_ABS);
        ext_defs(f,LABSYM,EXPORT,sec->idx,EXT_DEF);
        exttrailer(f);

        if (!no_symbols) {
          /* symbol table */
          if (!hunk_onlyglobal)
            ext_defs(f,LABSYM,0,sec->idx,EXT_SYMB);
          /* line-debug */
          linedebug_hunk(f,&linedblist,num_linedb);
        }
        fw32(f,HUNK_END,1);
      }
    }
  }
  if (!wrotesec) {
    /* there was no section at all - dummy section size 0 */
#if defined(VASM_CPU_PPC)
    fw32(f,HUNK_PPC_CODE,1);
#else
    fw32(f,HUNK_CODE,1);
#endif
    fw32(f,0,1);
    fw32(f,HUNK_END,1);
  }
}


static void write_exec(FILE *f,section *sec,symbol *sym)
{
  section *s;

  sec = prepare_sections(sec,sym);

  /* write header */
  fw32(f,HUNK_HEADER,1);
  fw32(f,0,1);

  if (sec_cnt) {
    fw32(f,sec_cnt,1);    /* number of sections - no overlay support */
    fw32(f,0,1);          /* first section index: 0 */
    fw32(f,sec_cnt-1,1);  /* last section index: sec_cnt - 1 */

    /* write section sizes and memory flags */
    for (s=sec; s; s=s->next) {
      if (!(s->flags & SEC_DELETED))
        fwmemflags(f,s,(get_sec_size(s)+3)>>2);
    }

    /* section hunk loop */
    for (; sec; sec=sec->next) {
      if (!(sec->flags & SEC_DELETED)) {
      	uint32_t type;
        atom *a;
        struct list reloclist,linedblist;
        int num_linedb = 0;

        initlist(&reloclist);
        initlist(&linedblist);

        /* write hunk-type and size */
        if (!(type = scan_attr(sec))) {
          output_error(3,sec->attr);  /* section attributes not suppported */
          type = HUNK_DATA;  /* default */
        }
        fw32(f,type,1);

        if (type != HUNK_BSS) {
          /* write contents */
          utaddr pc,npc,size,i;

          size = databss ? file_size(sec) : get_sec_size(sec);
          fw32(f,(size+3)>>2,1);
          for (a=sec->first,pc=0; a!=NULL&&pc<size; a=a->next) {
            rlist *rl;

            npc = fwpcalign(f,a,sec,pc);

            if (genlinedebug && (a->type==DATA || a->type==SPACE)) {
              add_linedebug(&linedblist,(uint32_t)a->line,npc);
              ++num_linedb;
            }
            if (a->type == DATA) {
              fwdata(f,a->content.db->data,a->content.db->size);
              process_relocs(a->content.db->relocs,&reloclist,NULL,sec,npc);
            }
            else if (a->type == SPACE) {
              fwsblock(f,a->content.sb);
              process_relocs(a->content.sb->relocs,&reloclist,NULL,sec,npc);
            }
            else if (a->type == LINE && !genlinedebug) {
              add_linedebug(&linedblist,(uint32_t)a->content.srcline,npc);
              ++num_linedb;
            }

            pc = npc + atom_size(a,sec,npc);
          }
          if (type == HUNK_CODE && (pc&1) == 0)
            fwnopalign(f,pc);
          else
            fwalign(f,pc,4);
        }
        else /* HUNK_BSS */
          fw32(f,(get_sec_size(sec)+3)>>2,1);

        if (!kick1)
          reloc_hunk(f,HUNK_ABSRELOC32,1,&reloclist);
        reloc_hunk(f,HUNK_ABSRELOC32,0,&reloclist);
        if (!kick1)  /* RELRELOC32 works with short 16-bit offsets only! */
          reloc_hunk(f,HUNK_RELRELOC32,1,&reloclist);

        if (!no_symbols) {
          /* symbol table */
          if (!hunk_onlyglobal)
            ext_defs(f,LABSYM,0,sec->idx,EXT_SYMB);
          /* line-debug */
          linedebug_hunk(f,&linedblist,num_linedb);
        }
        fw32(f,HUNK_END,1);
      }
    }
  }
  else {
    /* no sections: create single code hunk with size 0 */
    fw32(f,1,1);
    fw32(f,0,1);
    fw32(f,0,1);
    fw32(f,0,1);
    fw32(f,HUNK_CODE,1);
    fw32(f,0,1);
    fw32(f,HUNK_END,1);
  }
}


static int common_args(char *p)
{
#if defined(VASM_CPU_M68K)
  if (!strcmp(p,"-kick1hunks")) {
    kick1 = 1;
    return 1;
  }
#endif
  if (!strcmp(p,"-linedebug")) {
    genlinedebug = 1;
    return 1;
  }
  if (!strcmp(p,"-keepempty")) {
    keep_empty_sects = 1;
    return 1;
  }
  return 0;
}


static int object_args(char *p)
{
  return common_args(p);
}


static int exec_args(char *p)
{
  if (!strcmp(p,"-databss")) {
    databss = 1;
    return 1;
  }
  return common_args(p);
}


int init_output_hunk(char **cp,void (**wo)(FILE *,section *,symbol *),
                     int (**oa)(char *))
{
  *cp = copyright;
  *wo = write_object;
  *oa = object_args;
  return 1;
}


int init_output_hunkexe(char **cp,void (**wo)(FILE *,section *,symbol *),
                        int (**oa)(char *))
{
  *cp = copyright;
  *wo = write_exec;
  *oa = exec_args;
  return 1;
}

#else

int init_output_hunk(char **cp,void (**wo)(FILE *,section *,symbol *),
                     int (**oa)(char *))
{
  return 0;
}


int init_output_hunkexe(char **cp,void (**wo)(FILE *,section *,symbol *),
                        int (**oa)(char *))
{
  return 0;
}
#endif
