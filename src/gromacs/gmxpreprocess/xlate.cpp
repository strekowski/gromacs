/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2017,2018,2019, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
#include "gmxpre.h"

#include "xlate.h"

#include <cctype>
#include <cstring>

#include <string>
#include <vector>

#include "gromacs/gmxpreprocess/fflibutil.h"
#include "gromacs/gmxpreprocess/grompp_impl.h"
#include "gromacs/gmxpreprocess/hackblock.h"
#include "gromacs/topology/residuetypes.h"
#include "gromacs/topology/symtab.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/strdb.h"

typedef struct {
    char *filebase;
    char *res;
    char *atom;
    char *replace;
} t_xlate_atom;

static void get_xlatoms(const std::string &filename, FILE *fp,
                        int *nptr, t_xlate_atom **xlptr)
{
    char          filebase[STRLEN];
    char          line[STRLEN];
    char          abuf[1024], rbuf[1024], repbuf[1024], dumbuf[1024];
    char         *_ptr;
    int           n, na, idum;
    t_xlate_atom *xl;

    fflib_filename_base(filename.c_str(), filebase, STRLEN);

    n  = *nptr;
    xl = *xlptr;

    while (get_a_line(fp, line, STRLEN))
    {
        na = sscanf(line, "%s%s%s%s", rbuf, abuf, repbuf, dumbuf);
        /* Check if we are reading an old format file with the number of items
         * on the first line.
         */
        if (na == 1 && n == *nptr && sscanf(rbuf, "%d", &idum) == 1)
        {
            continue;
        }
        if (na != 3)
        {
            gmx_fatal(FARGS, "Expected a residue name and two atom names in file '%s', not '%s'", filename.c_str(), line);
        }

        srenew(xl, n+1);
        xl[n].filebase = gmx_strdup(filebase);

        /* Use wildcards... */
        if (strcmp(rbuf, "*") != 0)
        {
            xl[n].res = gmx_strdup(rbuf);
        }
        else
        {
            xl[n].res = nullptr;
        }

        /* Replace underscores in the string by spaces */
        while ((_ptr = strchr(abuf, '_')) != nullptr)
        {
            *_ptr = ' ';
        }

        xl[n].atom    = gmx_strdup(abuf);
        xl[n].replace = gmx_strdup(repbuf);
        n++;
    }

    *nptr  = n;
    *xlptr = xl;
}

static void done_xlatom(int nxlate, t_xlate_atom *xlatom)
{
    int i;

    for (i = 0; (i < nxlate); i++)
    {
        sfree(xlatom[i].filebase);
        if (xlatom[i].res != nullptr)
        {
            sfree(xlatom[i].res);
        }
        sfree(xlatom[i].atom);
        sfree(xlatom[i].replace);
    }
    sfree(xlatom);
}

void rename_atoms(const char* xlfile, const char *ffdir,
                  t_atoms *atoms, t_symtab *symtab, const t_restp *restp,
                  bool bResname, ResidueType *rt, bool bReorderNum,
                  bool bVerbose)
{
    int           nxlate, a, i, resind;
    t_xlate_atom *xlatom;
    char          c, *rnm, atombuf[32], *ptr0, *ptr1;
    bool          bReorderedNum, bRenamed, bMatch;
    bool          bStartTerm, bEndTerm;

    nxlate = 0;
    xlatom = nullptr;
    if (xlfile != nullptr)
    {
        gmx::FilePtr fp = gmx::openLibraryFile(xlfile);
        get_xlatoms(xlfile, fp.get(), &nxlate, &xlatom);
    }
    else
    {
        std::vector<std::string> fns = fflib_search_file_end(ffdir, ".arn", FALSE);
        for (const auto &filename : fns)
        {
            FILE * fp = fflib_open(filename);
            get_xlatoms(filename, fp, &nxlate, &xlatom);
            gmx_ffclose(fp);
        }
    }

    for (a = 0; (a < atoms->nr); a++)
    {
        resind = atoms->atom[a].resind;

        bStartTerm = (resind == 0) || atoms->resinfo[resind].chainnum != atoms->resinfo[resind-1].chainnum;
        bEndTerm   = (resind >= atoms->nres-1) || atoms->resinfo[resind].chainnum != atoms->resinfo[resind+1].chainnum;

        if (bResname)
        {
            rnm = *(atoms->resinfo[resind].name);
        }
        else
        {
            rnm = *(atoms->resinfo[resind].rtp);
        }

        strcpy(atombuf, *(atoms->atomname[a]));
        bReorderedNum = FALSE;
        if (bReorderNum)
        {
            if (isdigit(atombuf[0]))
            {
                c = atombuf[0];
                for (i = 0; (static_cast<size_t>(i) < strlen(atombuf)-1); i++)
                {
                    atombuf[i] = atombuf[i+1];
                }
                atombuf[i]    = c;
                bReorderedNum = TRUE;
            }
        }
        bRenamed = FALSE;
        for (i = 0; (i < nxlate) && !bRenamed; i++)
        {
            /* Check if the base file name of the rtp and arn entry match */
            if (restp == nullptr ||
                gmx_strcasecmp(restp[resind].filebase, xlatom[i].filebase) == 0)
            {
                /* Match the residue name */
                bMatch = (xlatom[i].res == nullptr ||
                          (gmx_strcasecmp("protein-nterm", xlatom[i].res) == 0 &&
                           rt->namedResidueHasType(rnm, "Protein") && bStartTerm) ||
                          (gmx_strcasecmp("protein-cterm", xlatom[i].res) == 0 &&
                           rt->namedResidueHasType(rnm, "Protein") && bEndTerm) ||
                          (gmx_strcasecmp("protein", xlatom[i].res) == 0 &&
                           rt->namedResidueHasType(rnm, "Protein")) ||
                          (gmx_strcasecmp("DNA", xlatom[i].res) == 0 &&
                           rt->namedResidueHasType(rnm, "DNA")) ||
                          (gmx_strcasecmp("RNA", xlatom[i].res) == 0 &&
                           rt->namedResidueHasType(rnm, "RNA")));
                if (!bMatch)
                {
                    ptr0 = rnm;
                    ptr1 = xlatom[i].res;
                    while (ptr0[0] != '\0' && ptr1[0] != '\0' &&
                           (ptr0[0] == ptr1[0] || ptr1[0] == '?'))
                    {
                        ptr0++;
                        ptr1++;
                    }
                    bMatch = (ptr0[0] == '\0' && ptr1[0] == '\0');
                }
                if (bMatch && strcmp(atombuf, xlatom[i].atom) == 0)
                {
                    /* We have a match. */
                    /* Don't free the old atomname,
                     * since it might be in the symtab.
                     */
                    ptr0 = gmx_strdup(xlatom[i].replace);
                    if (bVerbose)
                    {
                        printf("Renaming atom '%s' in residue %d %s to '%s'\n",
                               *atoms->atomname[a],
                               atoms->resinfo[resind].nr,
                               *atoms->resinfo[resind].name,
                               ptr0);
                    }
                    atoms->atomname[a] = put_symtab(symtab, ptr0);
                    bRenamed           = TRUE;
                }
            }
        }
        if (bReorderedNum && !bRenamed)
        {
            atoms->atomname[a] = put_symtab(symtab, atombuf);
        }
    }

    done_xlatom(nxlate, xlatom);
}
