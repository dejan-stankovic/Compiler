// Copyright (C) 1985-1998 by Symantec
// Copyright (C) 2000-2009 by Digital Mars
// All Rights Reserved
// http://www.digitalmars.com
// Written by Walter Bright
/*
 * This source file is made available for personal use
 * only. The license is in /dmd/src/dmd/backendlicense.txt
 * or /dm/src/dmd/backendlicense.txt
 * For any other uses, please contact Digital Mars.
 */

#include        <stdio.h>
#include        <string.h>
#include        <ctype.h>
#if !HOST_UNIX && !(linux || __APPLE__ || __FreeBSD__ || __OpenBSD__)
#include        <io.h>
#include        <share.h>
#endif
#include        <stdlib.h>
#include        <fcntl.h>
#include        <time.h>
#if M_UNIX || HOST_UNIX || linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
#include        <sys/stat.h>
#include        <unistd.h>
#else
#include        <sys\stat.h>
#endif
#include        "cc.h"
#include        "parser.h"
#include        "global.h"
#include        "filespec.h"
#include        "token.h"
#include        "scdll.h"
#include        "html.h"
#include        "outbuf.h"

static char __file__[] = __FILE__;      /* for tassert.h                */
#include        "tassert.h"

int loadline (FILE *,unsigned char **,unsigned char *);
STATIC void getcmd_filename (char **pname,const char *ext);
STATIC void file_openread(const char *f,blklst *b);

static int lastlinnum;
int includenest;

#if _WIN32
static list_t file_list;
#endif

// Default file I/O buffer size
#if _WIN32
#define FILEBUFSIZE     0x8000  // 32k disk buffer
#elif __INTSIZE == 4            // if 32 bit program
#define FILEBUFSIZE     0x4000  // 16k disk buffer
#elif _WINDLL
#define FILEBUFSIZE     0x4000
#else
#define FILEBUFSIZE     (_cpumode ? 0x4000 : 1024)      // small buffer for real mode
#endif

// File name extensions
#if M_UNIX || M_XENIX || linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
char ext_obj[] = ".o";
#else
char ext_obj[] = ".obj";
#endif
char ext_i[]   = ".i";
char ext_dep[] = ".dep";
char ext_lst[] = ".lst";
char ext_hpp[] = ".hpp";
char ext_c[]   = ".c";
char ext_cpp[] = ".cpp";
char ext_sym[] = ".sym";
char ext_tdb[] = ".tdb";
#if HTOD
char ext_dmodule[]   = ".d";
#endif

/*********************************
 * Open file for writing.
 * Input:
 *      f ->            file name string
 *      mode ->         open mode string
 * Returns:
 *      file stream pointer, or NULL if error
 */

FILE *file_openwrite(const char *name,const char *mode)
{   FILE *stream;
    char *newname;

    if (name)
    {   newname = file_nettranslate(name,mode);
        stream = fopen(newname,mode);

#if !((__SMALL__ || __MEDIUM__) && __INTSIZE == 2)
        // Adjust buffer size upwards
        if (stream && setvbuf(stream,NULL,_IOFBF,FILEBUFSIZE))
        {   fclose(stream);
            stream = NULL;              // buffer adjust failed
        }
#endif

        if (!stream)
            cmderr(EM_open_output,newname);     // error opening output file
    }
    else
        stream = NULL;
    return stream;
}

/*********************************************
 * Given a #include filename, search for the file.
 * If it exists, return a path to the file, and the time of the file.
 * Input:
 *      *pfilespec      filespec string
 *      flag            FQxxxx
 * Output:
 *      *pfilename      mem_malloc'd path of file, if found
 * Returns:
 *      !=0 if file is found
 */

#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
static list_t incfil_fndNdir;
#endif

int file_qualify(char **pfilename,int flag)
{   char *p;
    char *fname;
    char *pext;
    char *newname;
    blklst *b;
    int result;
#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
    list_t __searchpath;
    int save_flag;

#else
#define __searchpath pathlist
#endif

    p = *pfilename;
    assert(p);

    //printf("file_qualify(file='%s',flag=x%x\n",p,flag);
    if (flag & FQtop)
    {
        *pfilename = mem_strdup(p);
        return 1;
    }

    if (config.flags3 & CFG3igninc && flag & FQcwd)
    {   flag &= ~FQcwd;
        flag |= FQpath;
    }

#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
    if (flag & FQqual)                  // if already qualified
        flag = (flag | FQcwd) & ~(FQpath|FQnext);
    if (flag & FQpath)
    {
        __searchpath = pathsyslist;
        save_flag = flag;
        flag = FQpath|FQsystem;
    }

    if (flag & FQnext)
    {
        list_t pl;
        if (!incfil_fndNdir || !list_next(incfil_fndNdir))
        {
            return 0;
        }
        for (pl=list_next(incfil_fndNdir); pl; pl=list_next(pl))
        {
            fname = filespecaddpath((char *)list_ptr(pl),p);
            result = file_exists(fname);
            if (result)         // if file exists
            {
                incfil_fndNdir = pl;
                *pfilename = fname;
                return result;
            }
        }
        return 0;
    }
retry:
#else
    if (flag & FQqual)                          // if already qualified
        flag = (flag | FQcwd) & ~FQpath;
#endif

    pext = NULL;
    while (1)
    {
#if _MSDOS || __OS2__ || _WIN32 || M_UNIX || M_XENIX
        // If file spec is an absolute, rather than relative, file spec
        if (*p == '/' || *p == '\\' || (*p && p[1] == ':'))
            flag = FQcwd;       // don't look at paths
#endif
        switch (flag & (FQcwd | FQpath))
        {
            case FQpath:
                if (__searchpath)
                    break;
                /* FALL-THROUGH */
            case FQcwd | FQpath:                /* check current directory first */
                b = cstate.CSfilblk;
                if (b)
                {   char *p2,c;

                    /* Look for #include file relative to directory that
                       enclosing file resides in.
                     */
                    p2 = filespecname(blklst_filename(b));
                    c = *p2;
                    *p2 = 0;
                    fname = filespecaddpath(blklst_filename(b),p);
                    *p2 = c;
                }
                else
                {
            case FQcwd:     // Look relative to current directory
                    fname = mem_strdup(p);
                }
                //dbg_printf("1 stat('%s')\n",fname);
                result = file_exists(fname);
                if (result)             // if file exists
                {
                    *pfilename = fname;
                    return result;
                }
                mem_free(fname);
                break;
            default:
                assert(0);
        }
        if (flag & FQpath)      // if look at include path
        {   list_t pl;

            for (pl = __searchpath; pl; pl = list_next(pl))
            {
                fname = filespecaddpath((char *)list_ptr(pl),p);
                //dbg_printf("2 stat('%s')\n",fname);
                result = file_exists(fname);
                if (result)             // if file exists
                {
#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
                    incfil_fndNdir = pl;
#endif
                    *pfilename = fname;
                    return result;
                }
                mem_free(fname);
            }
        }
#if 1
        if (filespeccmp(filespecdotext(p),ext_hpp) == 0)
        {   // Chop off the "pp" and try again
            pext = p + strlen(p) - 2;
            *pext = 0;
        }
        else
        {   if (pext)
                *pext = 'p';            // restore ".hpp"
            break;
        }
#else
        break;
#endif
    }
#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
    if (__searchpath == pathsyslist)
    {
        __searchpath = pathlist;
        flag = save_flag;
        goto retry;
    }
#endif
    return 0;                   // not found
}

/*********************************************
 * Open a new file for input.
 * Watch out for open failures!
 * Input:
 *      p ->            filespec string (NULL for fin)
 *      bl ->           blklst structure to fill in
 *      flag            FQxxxx
 * Output:
 *      bl ->           newly opened file data
 */

void afopen(char *p,blklst *bl,int flag)
{
    //printf("afopen(%p,'%s',flag=x%x)\n",p,p,flag);
#if HTOD
    htod_include(p, flag);
#endif
    if (!file_qualify(&p,flag))
        err_fatal(EM_open_input,p);             // open failure
    bl->BLsrcpos.Sfilptr = filename_indirect(filename_add(p));
    sfile_debug(&srcpos_sfile(bl->BLsrcpos));
    srcpos_sfile(bl->BLsrcpos).SFflags |= (flag & FQtop) ? SFtop : 0;
    file_openread(p,bl);
    if (cstate.CSfilblk)
    {   sfile_debug(&srcpos_sfile(cstate.CSfilblk->BLsrcpos));
        list_append(&srcpos_sfile(cstate.CSfilblk->BLsrcpos).SFfillist,*bl->BLsrcpos.Sfilptr);
    }

#if !TARGET_68000
    if (configv.verbose)
        NetSpawnFile(p,(flag & FQsystem) ? -(includenest + 1) : includenest);
    includenest++;
    if (configv.verbose == 2)
    {   int i;
        char buffer[32];

        memset(buffer,' ',sizeof(buffer));
        i = (includenest < sizeof(buffer)) ? includenest : sizeof(buffer) - 1;
        buffer[i] = 0;
        dbg_printf("%s'%s'\n",buffer,p);
    }
#endif
#if !SPP
    if (fdep && !(flag & FQsystem))
    {
        fprintf(fdep, "%s ", p);
    }
#endif
    mem_free(p);
}

/*********************************************
 * Determine the source file name.
 * Input:
 *      what the user gave us for a source name
 * Returns:
 *      malloc'd name
 */

char *file_getsource(const char *iname)
{
    int i;
    char *n;
    char *p;
    size_t len;

#if TARGET_MAC
    static char ext[][4] = { "cpp","cp","c" };
#elif M_UNIX || M_XENIX
    static char ext[][4] = { "cpp","cxx","c", "C", "cc", "c++" };
#else
    static char ext[][5] = { "cpp","c","cxx","htm","html" };
#endif

    // Generate file names
    if (!iname || *iname == 0)
        cmderr(EM_nosource);            // no input file specified

    len = strlen(iname);
    n = (char *) malloc(len + 6);       // leave space for .xxxx0
    assert(n);
    strcpy(n,iname);
    p = filespecdotext(n);
    if (!*p)    // if no extension
    {
        for (i = 0; i < arraysize(ext); i++)
        {   *p = '.';
            strcpy(p + 1,ext[i]);
            if (file_exists(n) & 1)
                break;
            *p = 0;
        }
    }
    return n;
}

/***************************************
 * Twiddle with file names, open files for I/O.
 */

void file_iofiles()
{
    // Switch into mem space
    char *p = finname;
    finname = mem_strdup(p);
    free(p);

    assert(finname);
    filename_add(finname);

#if SPP
    if (!foutname)
        fout = stdout;
    else
    {
    if (filespeccmp(filespecdotext(foutname),ext_obj) == 0)
        // Ignore -o switch if it is a .obj filename
        foutname = (char*)"";
#if M_UNIX || M_XENIX
    if (*foutname)
#endif
    {
        getcmd_filename(&foutname,ext_i);
        fout = file_openwrite(foutname,"w");
    }
#if M_UNIX || M_XENIX
    else
        fout = stdout;
#endif
    }
#else
    // See if silly user specified output file name for -HF with -o
    if (fsymname && !*fsymname && filespeccmp(filespecdotext(foutname),ext_sym) == 0)
    {   fsymname = foutname;
        foutname = (char*)"";
        config.flags2 |= CFG2noobj;
    }

    getcmd_filename(&foutname,ext_obj);
    getcmd_filename(&fdepname,ext_dep);
    getcmd_filename(&flstname,ext_lst);
    getcmd_filename(&fsymname,ext_sym);
#if HTOD
    getcmd_filename(&fdmodulename,ext_dmodule);
#endif

    if (!ftdbname || !ftdbname[0])
        ftdbname = (char*)"symc.tdb";
    getcmd_filename(&ftdbname,ext_tdb);

#ifdef DEBUG
    printf("source <= '%s' obj => '%s' dep => '%s' lst => '%s' sym => '%s' tdb => '%s'\n",
        finname,foutname,fdepname,flstname,fsymname,ftdbname);
#endif

    // Now open the files
#if HTOD
    fdmodule = file_openwrite(fdmodulename,"w");
#else
    objfile_open(foutname);
    fdep = file_openwrite(fdepname,"w");
    flst = file_openwrite(flstname,"w");

    if (0 && fdep)
    {   // Build entire makefile line
        fprintf(fdep, "%s : ", foutname);
    }
#endif
#endif
}

/********************************
 * Generate output file name with default extension.
 * If no file name, use finname as the basis for it.
 * Input:
 *      finname         input file name
 *      foutdir         output file default directory
 */

STATIC void getcmd_filename(char **pname,const char *ext)
{   char *p;

#ifdef DEBUG
    assert(*ext == '.');
#endif
    ext++;                              // skip over '.'
    p = *pname;
    if (p)
    {   char *n;

        n = filespecforceext(filespecname(finname),ext);
        if (*p)
        {
            if (file_isdir(p))
                p = filespecaddpath(p,n);
            else if (foutdir && *p != '\\' && *p != '/' && p[1] != ':')
            {   mem_free(n);
                n = filespecaddpath(foutdir,p);
                p = filespecdefaultext(n,ext);
            }
            else
                p = filespecdefaultext(p,ext);
            mem_free(n);
        }
        else if (foutdir)
        {
            p = filespecaddpath(foutdir,n);
            mem_free(n);
        }
        else
            p = n;


        if (!filename_cmp(finname,p))
        {   *pname = NULL;
            cmderr(EM_mult_files,finname);      // duplicate file names
        }
        *pname = p;
    }
}

#if INDIVFILEIO

/******************************
 * Read in source file.
 */

STATIC void file_openread(const char *name,blklst *b)
{   unsigned char *p;
    unsigned long size;
    char *newname;
    int fd;

    //dbg_printf("file_openread('%s')\n",name);
    assert(__INTSIZE == 4);

    newname = file_nettranslate(name,"rb");
#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
    fd = open(newname,O_RDONLY,S_IREAD);
#else
    fd = _sopen(newname,O_RDONLY | O_BINARY,_SH_DENYWR);
#endif
    if (fd == -1)
        err_fatal(EM_open_input,newname);               // open failure

    /*  1:      so we can index BLtext[-1]
        2:      so BLtext is 2 behind BLbufp, allowing for addition of
                \n at end of file followed by 0, without stepping on ^Z
        2:      allow room for appending LF ^Z
     */
    size = os_file_size(fd);

    // If it is an HTML file, read and preprocess it
    char *dotext = filespecdotext(name);
    if (filespeccmp(dotext, ".htm") == 0 ||
        filespeccmp(dotext, ".html") == 0)
    {
        unsigned char *p;

        p = (unsigned char *) util_malloc(size + 1, 1);
        if (read(fd,p,size) != size)
            err_fatal(EM_eof);                  // premature end of source file
        close(fd);
        p[size] = 0;                            // make sure it's terminated

        Outbuffer buf;
        Html h(name, p, size);

        buf.reserve(3 + size + 2);
        buf.writeByte(0);
        buf.writeByte(0);
        buf.writeByte(0);
        h.extractCode(&buf);                    // preprocess
        size = buf.size() - 3;

        b->BLbuf = buf.buf;
        b->BLtext = b->BLbuf + 1;
        b->BLbufp = b->BLbuf + 3;
#if !__GNUC__
        buf = NULL;
#endif
    }
    else
    {
        b->BLbuf = (unsigned char *) util_malloc(1 + 2 + size + 2,1);
        memset(b->BLbuf,0,3);
        b->BLtext = b->BLbuf + 1;
        b->BLbufp = b->BLbuf + 3;

        if (read(fd,b->BLbufp,size) != size)
            err_fatal(EM_eof);                  // premature end of source file
        close(fd);
    }

    // Put a ^Z past the end of the buffer as a sentinel
    // (So buffer is guaranteed to end in ^Z)
    b->BLbufp[size] = 0x1A;

    // Scan buffer looking for terminating ^Z
#if __SC__ && TX86 && __INTSIZE == 4
    // Guarantee alignment
    assert(((long)(b->BLbufp) & 3) == 3);
    __asm
    {
        // This saves about 1/4 cycle per char over memchr()
        // due to a 0x1A must be there
        mov     ECX,b
        mov     ECX,BLbufp[ECX]
        mov     AL,0x1A
        cmp     [ECX],AL
        jz      Lf
        inc     ECX                     // to 4 byte alignment
L1:     mov     EDX,[ECX]
        add     ECX,4
        cmp     DL,AL
        jz      Lf1
        cmp     DH,AL
        jz      Lf2
        shr     EDX,16
        cmp     DL,AL
        jz      Lf3
        cmp     DH,AL
        jnz     L1
        inc     ECX
Lf3:    inc     ECX
Lf2:    inc     ECX
Lf1:    sub     ECX,4
Lf:     mov     p,ECX
    }
#else
    p = (unsigned char *)memchr(b->BLbufp,0x1A,size + 1);
    assert(p);
#endif

    // File must end in LF. If it doesn't, make it.
    if (p[-1] != LF)
    {
#if !HOST_MAC           // Mac editor does not always terminate last line
        if (ANSI && !CPP)
            lexerr(EM_no_nl);   // file must be terminated by '\n'
#endif
        p[0] = LF;
        p[1] = 0x1A;
    }
}

#else

/*********************************
 * Open file for reading.
 * Try to use a buffer size as big as the file to read, so the file
 * is read in one go.
 * Input:
 *      f ->            file name string
 * Returns:
 *      file stream pointer, or NULL if error
 */

STATIC void file_openread(const char *f,blklst *b)
{   FILE *fp;
    char *newname;

    assert(f);
    newname = file_nettranslate(f,"rb");

#if _WIN32
    fp = _fsopen(newname,"rb",_SH_DENYWR)
#else
    fp = fopen(newname,"rb");
#endif
    if (fp)
    {   unsigned long size;

        size = os_file_size(fileno(fp));
        if (size == -1L)
            goto Lerr;

#if __INTSIZE == 2
        if (size > FILEBUFSIZE)
            size = FILEBUFSIZE;         // if memory is tight
#endif

        // Adjust buffer size
        if (setvbuf(fp,NULL,_IOFBF,size))
        {
         Lerr:
            fclose(fp);
            fp = NULL;          // buffer adjust failed
        }
    }

    if (!fp)
        cmderr(EM_open_input,f);        // if bad open
    b->BLstream = fp;
}

#endif

#if INDIVFILEIO

/***********************************
 * Read next line from current input file.
 * Input:
 *      bl              currently open file
 * Returns:
 *      0               if no more input
 *      !=0             line buffer filled in
 */

int readln()
{   unsigned char c;
    unsigned char *ps,*p;
    int tristart = 0;
    blklst *b = bl;

    assert(bl);
    b->BLsrcpos.Slinnum++;              // line counter

#if TX86 && !(linux || __APPLE__ || __FreeBSD__ || __OpenBSD__)
        __asm
        {
                mov     ESI,b
                xor     DL,DL
                mov     EDI,[ESI].BLbuf
                mov     ECX,0x0D0A      ;CH = CR, CL = LF
                inc     EDI
                mov     [ESI].BLtext,EDI
                mov     btextp,EDI
                mov     ESI,[ESI].BLbufp
        L1:
                mov     AL,[ESI]
                cmp     AL,0x1A
                jnz     L4
        }
                includenest--;
                if (configv.verbose)
                    NetSpawnFile(blklst_filename(b),kCloseLevel);
#if HTOD
                htod_include_pop();
#endif
                return FALSE;
        __asm
        {
        L3:     mov     3[EDI],DL
                mov     AL,4[ESI]
                add     ESI,4
                add     EDI,4

        L4:     cmp     AL,CL
                jz      L10
                mov     DL,1[ESI]
                mov     [EDI],AL

                cmp     DL,CL
                jz      L11
                mov     AL,2[ESI]
                mov     1[EDI],DL

                cmp     AL,CL
                jz      L12
                mov     DL,3[ESI]
                mov     2[EDI],AL

                cmp     DL,CL
                jnz     L3

                cmp     AL,CH
                jnz     L13
                dec     EDI
        L13:    add     ESI,4
                add     EDI,3
                jmp     Lx

        L12:    cmp     DL,CH
                jnz     L14
                dec     EDI
        L14:    add     ESI,3
                add     EDI,2
                jmp     Lx

        L11:    cmp     AL,CH
                jnz     L15
                dec     EDI
        L15:    add     ESI,2
                inc     EDI
                jmp     Lx

        L10:    cmp     DL,CH
                jnz     L16
                dec     EDI
        L16:    inc     ESI

        Lx:     mov     p,EDI
                mov     ps,ESI

        }
#else
        b->BLtext = b->BLbuf + 1;               // +1 so we can bl->BLtext[-1]
        btextp = b->BLtext;             // set to start of line
        p = btextp;
        ps = b->BLbufp;
    L1:
        c = *ps++;
        if (c == 0x1A)
        {
            includenest--;
            if (configv.verbose)
                NetSpawnFile(blklst_filename(b),kCloseLevel);
#if HTOD
            htod_include_pop();
#endif
            return FALSE;
        }
        while (c != LF)
        {   if (c != CR)
                *p++ = c;               // store char in input buffer
            c = *ps++;
        }
#endif
        {
                if (TRIGRAPHS)
                {   // Do trigraph translation
                    // BUG: raw string literals do not undergo trigraph translation
                    static char trigraph[] = "=(/)'<!>-";
                    static char mongraph[] = "#[\\]^{|}~"; // translation of trigraph
                    int len;
                    unsigned char *s,*sn;

                    len = p - btextp;
                    // tristart is so we don't scan twice for trigraphs
                    for (s = btextp + tristart;
                         (sn = (unsigned char *)memchr(s,'?',len)) != NULL; )
                    {   unsigned char *q;

                        len -= sn - s;          // len = remaining length
                        s = sn;
                        if (*++s == '?' &&
                            (q = (unsigned char *) strchr(trigraph,s[1])) != NULL)
                        {   s[-1] = mongraph[q - (unsigned char *) trigraph];
                            len -= 2;
                            p -= 2;
                            memmove(s,s + 2,len);
                        }
                    }
                    tristart = p - btextp;
                }

                // Translate trailing CR-LF to LF
                //if (p > btextp && p[-1] == '\r')
                //    p--;

                // Look for backslash line splicing
                if (p[-1] == '\\')
                {
                    // BUG: backslash line splicing does not happen in raw strings
                    if (ismulti(p[-2]))
                    {   // Backslash may be part of multibyte sequence
                        unsigned char *s;

                        for (s = btextp; s < p; s++)
                        {
                            if (ismulti(*s))
                            {   s++;
                                if (s == p - 1) // backslash is part of multibyte
                                    goto L5;    // not a line continuation
                            }
                        }
                    }
                    p--;
                    b->BLsrcpos.Slinnum++;
#if TX86 && !__GNUC__
                    _asm
                    {
                        mov     EDI,p
                        mov     ESI,ps
                        xor     DL,DL
                        mov     ECX,0x0D0A      ;CH = CR, CL = LF
                    }
#endif
                    goto L1;
                }
                else
                {
                L5:
                    p[0] = LF;
                    p[1] = 0;
                    b->BLbufp = ps;
                    return TRUE;
                }
        }
}

#else

/***********************************
 * Read next line from current input file.
 * Special cases to consider:
 *      1) line buffer array size exceeded
 *      2) end of file encountered
 *      3) end of source input (no more files to read)
 *      4) reading in nulls
 * Input:
 *      bl              currently open file
 * Output:
 *      bl->linbuf[] =  next line of input file
 *      bl->linp =      bl->linbuf
 *      bl->linnum      incremented
 * Returns:
 *      0               if no more input (case 3)
 */

int readln()
{ int c;
  unsigned char *p,*ptop,*pstart;
  int tristart = 0;

  assert(bl);
  bl->BLsrcpos.Slinnum++;               // line counter
  p = btextp = bl->BLtext;              /* set to start of line         */
  pstart = p;

  /* Allow 2 extra here, 1 for character and 1 for terminating 0        */
  ptop = p + bl->BLtextmax - 2;         /* end of buffer                */

  while (1)
  {
#if __ZTC__ && M_I86
    c = loadline(bl->BLstream,&p,ptop);
#else
    c = fgetc(bl->BLstream);            /* get char from input file     */
#endif
    switch (c)
    {
        case '\n':
        L2:
            if (TRIGRAPHS)
            {   // Do trigraph translation
                // BUG: raw string literals do not undergo trigraph translation
                static char trigraph[] = "=(/)'<!>-";
                static char mongraph[] = "#[\\]^{|}~";  /* translation of trigraph */
                int len;
                unsigned char *s,*sn;

                len = p - bl->BLtext;
                /* tristart is so we don't scan twice for trigraphs     */
                for (s = bl->BLtext + tristart;
                     (sn = (unsigned char *)memchr(s,'?',len)) != NULL; )
                {   unsigned char *q;

                    len -= sn - s;              /* len = remaining length */
                    s = sn;
                    if (*++s == '?' &&
                        (q = (unsigned char *) strchr(trigraph,s[1])) != NULL)
                    {   s[-1] = mongraph[q - (unsigned char *) trigraph];
                        len -= 2;
                        p -= 2;
                        memmove(s,s + 2,len);
                        if (pstart > s)
                            pstart -= 2;
                    }
                }
                tristart = p - bl->BLtext;
            }

            // Translate trailing CR-LF to LF
            if (p > pstart && p[-1] == '\r')
                p--;

            // Look for backslash line splicing
            if (p > pstart && p[-1] == '\\')
            {
                // BUG: backslash line splicing does not happen in raw strings
                if (p > pstart + 1 && ismulti(p[-2]))
                {   // Backslash may be part of multibyte sequence
                    unsigned char *s;

                    for (s = pstart; s < p; s++)
                    {
                        if (ismulti(*s))
                        {   s++;
                            if (s == p - 1)     // backslash is part of multibyte
                                goto L5;        // not a line continuation
                        }
                    }
                }

                p--;
                pstart = p;
                bl->BLsrcpos.Slinnum++;
                continue;
            }
        L5:
            *p++ = c;
            *p = 0;
            return TRUE;

        default:
            *p++ = c;                   /* store char in input buffer   */
            break;

#if !(__ZTC__ && M_I86)                 // already handled by loadline()
        case 26:                        /* ^Z means EOF                 */
#endif
        case EOF:
            if (p != bl->BLtext)        // if we read in some chars
            {   *p = 0;                 // terminate line so it'll print
#if !HOST_MAC                   // Mac editor does not alway terminate last line
                if (ANSI && !CPP)
                    lexerr(EM_no_nl);
#endif
                c = '\n';               /* fake a '\n'                  */
                goto L2;
            }
            includenest--;
            fclose(bl->BLstream);       /* close input file             */
            if (configv.verbose)
                NetSpawnFile(blklst_filename(bl),kCloseLevel);
#if HTOD
            htod_include_pop();
#endif
            return FALSE;
    }
    if (p > ptop)                       /* too many chars in line       */
    {   /* allocate a new text buffer */
        int istart;
        long newmax;

        assert(p == bl->BLtext + bl->BLtextmax - 1);
#ifdef DEBUG
        assert(p == ptop + 1);
        assert(pstart <= p);
#endif
        istart = pstart - bl->BLtext;

        newmax = bl->BLtextmax * 2L;
#if __INTSIZE == 2
        #define TEXTMAX 0xFFF0
        if (newmax >= TEXTMAX)
        {
            if (bl->BLtextmax == TEXTMAX)
                err_fatal(EM_max_macro_text,"line",TEXTMAX);    // too big
            newmax = TEXTMAX;
        }
#endif
        bl->BLtext = (unsigned char *) util_realloc(bl->BLtext,1,newmax);
        btextp = bl->BLtext;
        pstart = btextp + istart;
        p = btextp + bl->BLtextmax - 1;         /* set to end of buffer */
        bl->BLtextmax = newmax;
        ptop = bl->BLtext + (int)newmax - 2;    /* end of buffer        */
    }
  }
}

#endif

/***********************************
 * Write out current line, and draw a ^
 * under the current position of the line pointer.
 * Input:
 *      fstream =       output stream pointer
 *      bl->            input file data
 */

#define line_out TRUE
void wrtpos(FILE *fstream)
{   char *p,*ptop,*fname;
    int fline;
    int aline;
    blklst *b;
    Srcpos sp;

    sp = token_linnum();
    fline = sp.Slinnum;
    b = cstate.CSfilblk;
    if (!b)                             /* no data to read              */
    {
        if (fline)
            fname = srcpos_name(sp);
        else
            fname = finname;
        aline = 0;
    }
    else
    {
        fname = srcpos_name(sp);
        p = (char *) b->BLtext;
        ptop = (char *) ((b == bl) ? btextp : b->BLtextp);
        aline = b->BLsrcpos.Slinnum;    /* actual line number           */
    }
    if (line_out && aline == fline)     /* if on right line             */
    {
        if (config.flags2 & CFG2expand)
        {
            wrtexp(fstream);            /* write expanded output        */
            p = eline;
            ptop = p + elini;
        }
#if M_UNIX || M_XENIX || linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
        else if (fstream == stderr)     /* line already written to .LST */
#else
        else if (fstream == stdout)     /* line already written to .LST */
#endif
            wrtlst(fstream);            // write listing line
    }

    if (fline)
    {
     if (line_out && aline == fline && *p)
     {                                  /* only if on right line        */
         if (ptop - p >= 2)
            ptop -= 2;
         for (; *p != '\n' && p < ptop; p++)
            fputc(((*p == '\t') ? '\t' : ' '),fstream);
         fprintf(fstream,"^\n");
     }
     fprintf(fstream,dlcmsgs(EM_line_format),fname,fline);
    }
}


/**********************************
 * Send current line to stream.
 * Input:
 *      fstream =       output stream pointer
 *      bl->            input file data
 */

void wrtlst(FILE *fstream)
{ blklst *b;

  b = cstate.CSfilblk;
  if (b)                                /* if data to read              */
  {     char c,*p;

        for (p = (char *) b->BLtext; (c = *p) != 0; p++)
        {   if (isillegal(c))
                c = ' ';
            if (c != '\n' && c != '\r')
                fputc(c,fstream);
        }
        crlf(fstream);
        fflush(fstream);
  }
}

/***********************************
 * Send progress report.
 */

void file_progress()
{
    if (controlc_saw)
        util_exit(EXIT_BREAK);
#if USEDLLSHELL
    if (configv.verbose)
    {   blklst *b;

        b = cstate.CSfilblk;
        if (NetSpawnProgress(b ? b->BLsrcpos.Slinnum : kNoLineNumber) != NetSpawnOK)
            err_exit();
    }
#endif
}

/***********************************
 * Net translate filename.
 */

#if _WIN32 && _WINDLL

char *file_nettranslate(const char *filename,const char *mode)
{   char *newname;
    static int nest;

    nest++;
    newname = NetSpawnTranslateFileName((char *)filename,(char *)mode);
    if (!newname)
    {   if (nest == 1)
            err_exit();                 // abort without message
    }
    else
        list_append(&file_list,newname);
    nest--;
    return newname;
}

#endif

/************************************
 * Delete file.
 */

void file_remove(char *fname)
{   char *newname;

    if (fname)
    {   newname = NetSpawnTranslateFileName(fname,"w");
        if (newname)
        {   remove(newname);    // delete file
            NetSpawnDisposeFile(newname);
        }
    }
}

/***********************************
 * Do a stat on a file.
 */

int file_stat(const char *fname,struct stat *pbuf)
{
#if _WIN32 && _WINDLL
    int result;
    char *newname;

    newname = NetSpawnTranslateFileName((char *)fname,"rb");
    if (newname)
    {   result = stat(newname,pbuf);
        NetSpawnDisposeFile(newname);
    }
    else
        result = -1;
    return result;
#else
    return stat(fname,pbuf);
#endif
}

/*************************************
 * Determine if fname is a directory.
 * Returns:
 *      0       not a directory
 *      !=0     a directory
 */

int file_isdir(const char *fname)
{
    char c;
    int result;

    c = fname[strlen(fname) - 1];
    if (c == ':' || c == '/' || c == '\\')
        result = 2;
    else
        result = file_exists(fname) & 2;
    return result;
}

/**************************************
 * Determine if file exists.
 * Returns:
 *      0:      file doesn't exist
 *      1:      normal file
 *      2:      directory
 */

int file_exists(const char *fname)
{
#if __SC__
    int result;
    char *newname;

    newname = NetSpawnTranslateFileName((char *)fname,"rb");
    if (newname)
    {   result = os_file_exists(newname);
        NetSpawnDisposeFile(newname);
    }
    else
        result = 0;
    return result;
#elif HOST_UNIX || linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
    struct stat buf;

    return stat(fname,&buf) == 0;       /* file exists if stat succeeded */

#else
    return os_file_exists(fname);
#endif
}

/***********************************
 * Determine size of file.
 * Returns:
 *      -1L     file not found
 */

long file_size(const char *fname)
{
#if __SC__
    long result;
    char *newname;

    newname = NetSpawnTranslateFileName((char *)fname,"rb");
    if (newname)
    {   result = filesize(newname);
        NetSpawnDisposeFile(newname);
    }
    else
        result = -1L;
    return result;
#else
    long result;
    struct stat buf;

    if (file_stat(fname,&buf) != -1)
        result = buf.st_size;
    else
        result = -1L;
    return result;
#endif
}

/***********************************
 * Terminate use of all network translated filenames.
 */

void file_term()
{
#if _WIN32 && _WINDLL
    list_t fl;

    for (fl = file_list; fl; fl = list_next(fl))
        NetSpawnDisposeFile((char *)list_ptr(fl));
    list_free(&file_list,FPNULL);
#endif
    //printf("free(%p)\n",cstate.modname);
    free(cstate.modname);
    cstate.modname = NULL;
}

/*************************************
 * Translate input file name into an identifier unique to this module.
 * Returns:
 *      pointer to identifier we can use. Caller does not need to free it.
 */

#if !SPP

char *file_unique()
{
    if (!cstate.modname)
    {   char *p;
        size_t len;

        len = 2 + strlen(finname) + sizeof(long) * 3 + 1;
        p = (char *)malloc(len);
        //printf("malloc(%d) = %p\n",len,p);
        cstate.modname = p;
#if linux || __APPLE__ || __FreeBSD__ || __OpenBSD__
        snprintf(p,len,"__%s%lu",finname,getpid());
#else
        sprintf(p,"?%%%s%lu",finname,os_unique());
#endif
        assert(strlen(p) < len);
        p += 2;
        do
        {   if (!isalnum(*p))
                *p = '_';               // force valid identifier char
        } while (*++p);
    }
    return cstate.modname;
}

#endif