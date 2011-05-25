/* CM_TOPHITS: implementation of ranked list of top-scoring hits Based
 * closely on HMMER3's P7_TOPHITS but with some complexity removed
 * since we don't have to worry about domains nor with iterative
 * searches (yet) with CM hits, and some extra complexity for bridging
 * the gap with older versions of Infernal.
 *
 * Contents:
 *    1. The CM_TOPHITS object.
 *    2. Standard (human-readable) output of pipeline results.
 *    3. Tabular (parsable) output of pipeline results.
 *    4. Benchmark driver.
 *    5. Test driver.
 *    6. Copyright and license information.
 * 
 * EPN, Tue May 24 13:03:31 2011
 * SVN $Id: p7_tophits.c 3546 2011-05-23 14:36:44Z eddys $
 */
#include "esl_config.h"
#include "p7_config.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "easel.h"
#include "hmmer.h"
#include "funcs.h"
#include "structs.h"


/*****************************************************************
 * 1. The CM_TOPHITS object
 *****************************************************************/

/* Function:  cm_tophits_Create()
 * Synopsis:  Allocate a hit list.
 * Incept:    EPN, Tue May 24 13:11:06 2011
 *            SRE, Fri Dec 28 07:17:51 2007 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Allocates a new <CM_TOPHITS> hit list and return a pointer
 *            to it.
 *
 * Throws:    <NULL> on allocation failure.
 */
CM_TOPHITS *
cm_tophits_Create(void)
{
  CM_TOPHITS *h = NULL;
  int         default_nalloc = 256;
  int         status;

  ESL_ALLOC(h, sizeof(CM_TOPHITS));
  h->hit    = NULL;
  h->unsrt  = NULL;

  ESL_ALLOC(h->hit,   sizeof(CM_HIT *) * default_nalloc);
  ESL_ALLOC(h->unsrt, sizeof(CM_HIT)   * default_nalloc);
  h->Nalloc    = default_nalloc;
  h->N         = 0;
  h->nreported = 0;
  h->nincluded = 0;
  h->is_sorted = TRUE;        /* but only because there's 0 hits */
  h->hit[0]    = h->unsrt;    /* if you're going to call it "sorted" when it contains just one hit, you need this */
  return h;

 ERROR:
  cm_tophits_Destroy(h);
  return NULL;
}


/* Function:  cm_tophits_Grow()
 * Synopsis:  Reallocates a larger hit list, if needed.
 * Incept:    EPN, Tue May 24 13:13:52 2011
 *            SRE, Fri Dec 28 07:37:27 2007 [Janelia] (p7_tophits.c)
 *
 * Purpose:   If list <h> cannot hold another hit, doubles
 *            the internal allocation.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation failure; in this case,
 *            the data in <h> are unchanged.
 */
int
cm_tophits_Grow(CM_TOPHITS *h)
{
  void   *p;
  CM_HIT *ori    = h->unsrt;
  int     Nalloc = h->Nalloc * 2;    /* grow by doubling */
  int     i;
  int     status;

  if (h->N < h->Nalloc) return eslOK; /* we have enough room for another hit */

  ESL_RALLOC(h->hit,   p, sizeof(CM_HIT *) * Nalloc);
  ESL_RALLOC(h->unsrt, p, sizeof(CM_HIT)   * Nalloc);

  /* If we grow a sorted list, we have to translate the pointers
   * in h->hit, because h->unsrt might have just moved in memory. 
   */
  if (h->is_sorted) {
    for (i = 0; i < h->N; i++)
      h->hit[i] = h->unsrt + (h->hit[i] - ori);
  }

  h->Nalloc = Nalloc;
  return eslOK;

 ERROR:
  return eslEMEM;
}

/* Function:  cm_tophits_CreateNextHit()
 * Synopsis:  Get pointer to new structure for recording a hit.
 * Incept:    EPN, Tue May 24 13:14:12 2011
 *            SRE, Tue Mar 11 08:44:53 2008 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Ask the top hits object <h> to do any necessary
 *            internal allocation and bookkeeping to add a new,
 *            empty hit to its list; return a pointer to 
 *            this new <CM_HIT> structure for data to be filled
 *            in by the caller.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation error.
 */
int
cm_tophits_CreateNextHit(CM_TOPHITS *h, CM_HIT **ret_hit)
{
  CM_HIT *hit = NULL;
  int     status;

  if ((status = cm_tophits_Grow(h)) != eslOK) goto ERROR;
  
  hit = &(h->unsrt[h->N]);
  h->N++;
  if (h->N >= 2) h->is_sorted = FALSE;

  hit->name             = NULL;
  hit->acc              = NULL;
  hit->desc             = NULL;
  hit->sortkey          = 0.0;

  hit->start            = 1;
  hit->stop             = 1;
  hit->score            = 0.0;
  hit->pvalue           = 0.0;
  hit->evalue           = 0.0;
  hit->oasc             = 0.;
  hit->bestr            = 0;

  hit->ad               = NULL;
  hit->flags            = CM_HIT_FLAGS_DEFAULT;

  *ret_hit = hit;
  return eslOK;

 ERROR:
  *ret_hit = NULL;
  return status;
}

/* hit_sorter(): qsort's pawn, below */
static int
hit_sorter(const void *vh1, const void *vh2)
{
  CM_HIT *h1 = *((CM_HIT **) vh1);  /* don't ask. don't change. Don't Panic. */
  CM_HIT *h2 = *((CM_HIT **) vh2);

  if      (h1->sortkey < h2->sortkey) return  1;
  else if (h1->sortkey > h2->sortkey) return -1;
  else {
      int c = strcmp(h1->name, h2->name);
      if (c != 0)                       return c;
      else                              return  (h1->start < h2->start ? 1 : -1 );
  }
}

/* Function:  cm_tophits_Sort()
 * Synopsis:  Sorts a hit list.
 * Incept:    EPN, Tue May 24 13:30:23 2011
 *            SRE, Fri Dec 28 07:51:56 2007 [Janelia]
 *
 * Purpose:   Sorts a top hit list. After this call,
 *            <h->hit[i]> points to the i'th ranked 
 *            <CM_HIT> for all <h->N> hits.
 *
 * Returns:   <eslOK> on success.
 */
int
cm_tophits_Sort(CM_TOPHITS *h)
{
  int i;

  if (h->is_sorted)  return eslOK;
  for (i = 0; i < h->N; i++) h->hit[i] = h->unsrt + i;
  if (h->N > 1)  qsort(h->hit, h->N, sizeof(CM_HIT *), hit_sorter);
  h->is_sorted = TRUE;
  return eslOK;
}

/* Function:  cm_tophits_Merge()
 * Synopsis:  Merge two top hits lists.
 * Incept:    EPN, Tue May 24 13:30:39 2011
 *            SRE, Fri Dec 28 09:32:12 2007 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Merge <h2> into <h1>. Upon return, <h1>
 *            contains the sorted, merged list. <h2>
 *            is effectively destroyed; caller should
 *            not access it further, and may as well free
 *            it immediately.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation failure, and
 *            both <h1> and <h2> remain valid.
 */
int
cm_tophits_Merge(CM_TOPHITS *h1, CM_TOPHITS *h2)
{
  void    *p;
  CM_HIT **new_hit = NULL;
  CM_HIT  *ori1    = h1->unsrt;    /* original base of h1's data */
  CM_HIT  *new2;
  int      i,j,k;
  int      Nalloc = h1->Nalloc + h2->Nalloc;
  int      status;

  /* Make sure the two lists are sorted */
  if ((status = cm_tophits_Sort(h1)) != eslOK) goto ERROR;
  if ((status = cm_tophits_Sort(h2)) != eslOK) goto ERROR;

  /* Attempt our allocations, so we fail early if we fail. 
   * Reallocating h1->unsrt screws up h1->hit, so fix it.
   */
  ESL_RALLOC(h1->unsrt, p, sizeof(CM_HIT) * Nalloc);
  ESL_ALLOC (new_hit, sizeof(CM_HIT *)    * Nalloc);
  for (i = 0; i < h1->N; i++)
    h1->hit[i] = h1->unsrt + (h1->hit[i] - ori1);

  /* Append h2's unsorted data array to h1. h2's data begin at <new2> */
  new2 = h1->unsrt + h1->N;
  memcpy(new2, h2->unsrt, sizeof(CM_HIT) * h2->N);

  /* Merge the sorted hit lists */
  for (i=0,j=0,k=0; i < h1->N && j < h2->N ; k++)
    new_hit[k] = (hit_sorter(&h1->hit[i], &h2->hit[j]) > 0) ? new2 + (h2->hit[j++] - h2->unsrt) : h1->hit[i++];
  while (i < h1->N) new_hit[k++] = h1->hit[i++];
  while (j < h2->N) new_hit[k++] = new2 + (h2->hit[j++] - h2->unsrt);

  /* h2 now turns over management of name, acc, desc memory to h1;
   * nullify its pointers, to prevent double free.  */
  for (i = 0; i < h2->N; i++)
    {
      h2->unsrt[i].name = NULL;
      h2->unsrt[i].acc  = NULL;
      h2->unsrt[i].desc = NULL;
      h2->unsrt[i].ad   = NULL;
    }

  /* Construct the new grown h1 */
  free(h1->hit);
  h1->hit    = new_hit;
  h1->Nalloc = Nalloc;
  h1->N     += h2->N;
  /* and is_sorted is TRUE, as a side effect of cm_tophits_Sort() above. */
  return eslOK;
  
 ERROR:
  if (new_hit != NULL) free(new_hit);
  return status;
}

static int
integer_textwidth(long n)
{
  int w = (n < 0)? 1 : 0;
  while (n != 0) { n /= 10; w++; }
  return w;
}

/* Function:  cm_tophits_GetMaxPositionLength()
 * Synopsis:  Returns maximum position length in hit list (targets).
 * Incept:    EPN, Tue May 24 13:31:13 2011
 *            TJW, Mon May 24 14:16:16 EDT 2010 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Returns the length of the longest hit location (start/end)
 *               of all the registered hits, in chars. This is useful when
 *               deciding how to format output.
 *
 *            The maximum is taken over all registered hits. This
 *            opens a possible side effect: caller might print only
 *            the top hits, and the max name length in these top hits
 *            may be different than the max length over all the hits.
 *
 *            Used specifically for nhmmer output, so expects only one
 *            domain per hit
 *
 *            If there are no hits in <h>, or none of the
 *            hits have names, returns 0.
 */
int
cm_tophits_GetMaxPositionLength(CM_TOPHITS *h)
{
  int i, max;

  max = 0;
  for (i = 0; i < h->N; i++) {
    max = ESL_MAX(max, 
		  ESL_MAX(integer_textwidth(h->unsrt[i].start), integer_textwidth(h->unsrt[i].stop)));
  }
  return max;
}

/* Function:  cm_tophits_GetMaxNameLength()
 * Synopsis:  Returns maximum name length in hit list (targets).
 * Incept:    EPN, Tue May 24 13:34:12 2011
 *            SRE, Fri Dec 28 09:00:13 2007 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Returns the maximum name length of all the registered
 *            hits, in chars. This is useful when deciding how to
 *            format output.
 *            
 *            The maximum is taken over all registered hits. This
 *            opens a possible side effect: caller might print only
 *            the top hits, and the max name length in these top hits
 *            may be different than the max length over all the hits.
 *            
 *            If there are no hits in <h>, or none of the
 *            hits have names, returns 0.
 */
int
cm_tophits_GetMaxNameLength(CM_TOPHITS *h)
{
  int i, max;
  for (max = 0, i = 0; i < h->N; i++)
    if (h->unsrt[i].name != NULL) {
      max = ESL_MAX(max, strlen(h->unsrt[i].name));
    }
  return max;
}

/* Function:  cm_tophits_GetMaxAccessionLength()
 * Synopsis:  Returns maximum accession length in hit list (targets).
 * Incept:    EPN, Tue May 24 13:35:23 2011
 *            SRE, Tue Aug 25 09:18:33 2009 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Same as <cm_tophits_GetMaxNameLength()>, but for
 *            accessions. If there are no hits in <h>, or none
 *            of the hits have accessions, returns 0.
 */
int
cm_tophits_GetMaxAccessionLength(CM_TOPHITS *h)
{
  int i, max, n;
  for (max = 0, i = 0; i < h->N; i++)
    if (h->unsrt[i].acc != NULL) {
      n   = strlen(h->unsrt[i].acc);
      max = ESL_MAX(n, max);
    }
  return max;
}

/* Function:  cm_tophits_GetMaxShownLength()
 * Synopsis:  Returns max shown name/accession length in hit list.
 * Incept:    EPN, Tue May 24 13:35:50 2011
 *            SRE, Tue Aug 25 09:30:43 2009 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Same as <cm_tophits_GetMaxNameLength()>, but 
 *            for the case when --acc is on, where
 *            we show accession if one is available, and 
 *            fall back to showing the name if it is not.
 *            Returns the max length of whatever is being
 *            shown as the reported "name".
 */
int
cm_tophits_GetMaxShownLength(CM_TOPHITS *h)
{
  int i, max, n;
  for (max = 0, i = 0; i < h->N; i++)
  {
      if (h->unsrt[i].acc != NULL && h->unsrt[i].acc[0] != '\0')
    {
      n   = strlen(h->unsrt[i].acc);
      max = ESL_MAX(n, max);
    }
      else if (h->unsrt[i].name != NULL)
    {
      n   = strlen(h->unsrt[i].name);
      max = ESL_MAX(n, max);
    }
  }
  return max;
}

/* Function:  cm_tophits_Reuse()
 * Synopsis:  Reuse a hit list, freeing internals.
 * Incept:    EPN, Tue May 24 13:36:15 2011
 *            SRE, Fri Jun  6 15:39:05 2008 [Janelia] (p7_tophits.c)
 * 
 * Purpose:   Reuse the tophits list <h>; save as 
 *            many malloc/free cycles as possible,
 *            as opposed to <Destroy()>'ing it and
 *            <Create>'ing a new one.
 */
int
cm_tophits_Reuse(CM_TOPHITS *h)
{
  int i;

  if (h == NULL) return eslOK;
  if (h->unsrt != NULL) 
  {
      for (i = 0; i < h->N; i++)
    {
      if (h->unsrt[i].name != NULL) free(h->unsrt[i].name);
      if (h->unsrt[i].acc  != NULL) free(h->unsrt[i].acc);
      if (h->unsrt[i].desc != NULL) free(h->unsrt[i].desc);
      if (h->unsrt[i].ad != NULL)   FreeFancyAli(h->unsrt[i].ad);
    }
  }
  h->N         = 0;
  h->is_sorted = TRUE;
  h->hit[0]    = h->unsrt;
  return eslOK;
}

/* Function:  cm_tophits_Destroy()
 * Synopsis:  Frees a hit list.
 * Incept:    EPN, Tue May 24 13:36:49 2011
 *            SRE, Fri Dec 28 07:33:21 2007 [Janelia] (p7_tophits.c)
 */
void
cm_tophits_Destroy(CM_TOPHITS *h)
{
  int i;
  if (h == NULL) return;
  if (h->hit   != NULL) free(h->hit);
  if (h->unsrt != NULL) {
    for (i = 0; i < h->N; i++) {
      if (h->unsrt[i].name != NULL) free(h->unsrt[i].name);
      if (h->unsrt[i].acc  != NULL) free(h->unsrt[i].acc);
      if (h->unsrt[i].desc != NULL) free(h->unsrt[i].desc);
      if (h->unsrt[i].ad != NULL)   FreeFancyAli(h->unsrt[i].ad);
    }
    free(h->unsrt);
  }
  free(h);
  return;
}


/* Function:  cm_tophits_CloneHitFromResults()
 * Synopsis:  Add a new hit, a clone of a hit in a search_results_node_t object.
 * Incept:    EPN, Wed May 25 08:17:35 2011
 *
 * Purpose:   Create a new hit in the CM_TOPHITS object <th>
 *            and copy the information from the search_results_t
 *            object's <hidx>'th node into it. Return a pointer 
 *            to the new hit.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation error.
 */
int
cm_tophits_CloneHitFromResults(CM_TOPHITS *th, search_results_t *results, int hidx, CM_HIT **ret_hit)
{
  CM_HIT *hit = NULL;
  int     status;

  if ((status = cm_tophits_CreateNextHit(th, &hit)) != eslOK) goto ERROR;
  hit->start = results->data[hidx].start;
  hit->stop  = results->data[hidx].stop;
  hit->bestr = results->data[hidx].bestr;
  hit->score = results->data[hidx].score;

  *ret_hit = hit;
  return eslOK;

 ERROR:
  *ret_hit = NULL;
  return status;
}  

/* Function:  cm_tophits_ComputeEvalues()
 * Synopsis:  Compute E-values given effective database size.
 * Incept:    EPN, Tue Sep 28 05:26:20 2010
 *
 * Purpose:   After the cm pipeline has completed, the CM_TOPHITS object
 *            contains objects with p-values that haven't yet been
 *            converted to e-values.
 *
 * Returns:   <eslOK> on success.
 */
int
cm_tophits_ComputeEvalues(CM_TOPHITS *th, double eff_dbsize)
{
  int i; 

  for (i = 0; i < th->N ; i++) { 
    th->unsrt[i].evalue  = th->unsrt[i].pvalue * eff_dbsize;
    th->unsrt[i].sortkey = -th->unsrt[i].evalue;
  }
  return eslOK;
}

/*---------------- end, CM_TOPHITS object -----------------------*/

/*****************************************************************
 * 2. Standard (human-readable) output of pipeline results
 *****************************************************************/
/* Function:  cm_tophits_Threshold()
 * Synopsis:  Apply score and E-value thresholds to a hitlist before output.
 * Incept:    EPN, Tue May 24 13:44:13 2011
 *            SRE, Tue Dec  9 09:04:55 2008 [Janelia] (p7_tophits.c)
 *
 * Purpose:   After a pipeline has completed, go through it and mark all
 *            the targets and domains that are "significant" (satisfying
 *            the reporting thresholds set for the pipeline). 
 *            
 *            Also sets the final total number of reported and
 *            included targets, the number of reported and included
 *            targets in each target, and the size of the search space
 *            for per-domain conditional E-value calculations,
 *            <pli->domZ>. By default, <pli->domZ> is the number of
 *            significant targets reported.
 *
 *            If model-specific thresholds were used in the pipeline,
 *            we cannot apply those thresholds now. They were already
 *            applied in the pipeline. In this case all we're
 *            responsible for here is counting them (setting
 *            nreported, nincluded counters).
 *            
 * Returns:   <eslOK> on success.
 */
int
cm_tophits_Threshold(CM_TOPHITS *th, CM_PIPELINE *pli)
{
  int h;   /* counter */
  
  /* Flag reported, included targets (if we're using general thresholds) */
  if (! pli->use_bit_cutoffs) {
    for (h = 0; h < th->N; h++) {
      if (cm_pli_TargetReportable(pli, th->hit[h]->score, th->hit[h]->evalue)) {
	th->hit[h]->flags |= CM_HIT_IS_REPORTED;
	if (cm_pli_TargetIncludable(pli, th->hit[h]->score, th->hit[h]->evalue))
	  th->hit[h]->flags |= CM_HIT_IS_INCLUDED;
      }
    }
  }

  /* Count reported, included targets */
  th->nreported = 0;
  th->nincluded = 0;
  for (h = 0; h < th->N; h++) { 
    if (th->hit[h]->flags & CM_HIT_IS_REPORTED)  th->nreported++;
    if (th->hit[h]->flags & CM_HIT_IS_INCLUDED)  th->nincluded++;
  }
  
  return eslOK;
}


/* Function:  cm_tophits_Targets()
 * Synopsis:  Standard output format for a top target hits list.
 * Incept:    EPN, Tue May 24 13:48:29 2011
 *            SRE, Tue Dec  9 09:10:43 2008 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Output a list of the reportable top target hits in <th> 
 *            in human-readable ASCII text format to stream <ofp>, using
 *            final pipeline accounting stored in <pli>. 
 * 
 *            The tophits list <th> should already be sorted (see
 *            <cm_tophits_Sort()> and thresholded (see
 *            <cm_tophits_Threshold>).
 *
 * Returns:   <eslOK> on success.
 */
int
cm_tophits_Targets(FILE *ofp, CM_TOPHITS *th, CM_PIPELINE *pli, int textw)
{
  int    status;
  char   newness;
  int    h,i;
  int    namew;
  int    posw;
  int    descw;
  char  *showname;
  int    have_printed_incthresh = FALSE;

  char *namestr   = NULL;
  char *posstr    = NULL;

  /* when --acc is on, we'll show accession if available, and fall back to name */
  if (pli->show_accessions) namew = ESL_MAX(8, cm_tophits_GetMaxShownLength(th));
  else                      namew = ESL_MAX(8, cm_tophits_GetMaxNameLength(th));

  if (textw >  0)           descw = ESL_MAX(32, textw - namew - 61); /* 61 chars excluding desc is from the format: 2 + 22+2 +22+2 +8+2 +<name>+1 */
  else                      descw = 0;                               /* unlimited desc length is handled separately */

  posw = ESL_MAX(6, cm_tophits_GetMaxPositionLength(th));
  printf("posw: %d\n", posw);

  ESL_ALLOC(namestr, sizeof(char) * (namew+1));
  ESL_ALLOC(posstr,  sizeof(char) * (posw+1));

  for(i = 0; i < namew; i++) { namestr[i] = '-'; } namestr[namew] = '\0';
  for(i = 0; i < posw;  i++) { posstr[i]  = '-'; } posstr[posw]   = '\0';

  fprintf(ofp, "Hit scores:\n");
  /* The minimum width of the target table is 111 char: 47 from fields, 8 from min name, 32 from min desc, 13 spaces */
  fprintf(ofp, "  %9s %6s %-*s %*s %*s %s\n", "E-value",   " score", namew, (pli->mode == CM_SEARCH_SEQS ? "sequence":"model"), posw, "start", posw, "end", "description");
  fprintf(ofp, "  %9s %6s %-*s %*s %*s %s\n", "---------", "------", namew, namestr, posw, posstr, posw, posstr, "----------");
  
  for (h = 0; h < th->N; h++) { 
    if (th->hit[h]->flags & CM_HIT_IS_REPORTED)

      if (! (th->hit[h]->flags & CM_HIT_IS_INCLUDED) && ! have_printed_incthresh) {
	fprintf(ofp, "  ------ inclusion threshold ------\n");
	have_printed_incthresh = TRUE;
      }

    if (pli->show_accessions) { 
      /* the --acc option: report accessions rather than names if possible */
      if (th->hit[h]->acc != NULL && th->hit[h]->acc[0] != '\0') showname = th->hit[h]->acc;
      else                                                       showname = th->hit[h]->name;
    }
    else {
      showname = th->hit[h]->name;
    }

    newness = ' '; /* left-over from HMMER3 output, which uses this to mark new/dropped hits in iterative searches */
    fprintf(ofp, "%c %9.2g %6.1f %-*s %*ld %*ld ",
	    newness,
	    th->hit[h]->evalue,
	    th->hit[h]->score,
	    namew, showname,
	    posw, th->hit[h]->start,
	    posw, th->hit[h]->stop);

    if (textw > 0) fprintf(ofp, "%-.*s\n", descw, th->hit[h]->desc == NULL ? "" : th->hit[h]->desc);
    else           fprintf(ofp, "%s\n",           th->hit[h]->desc == NULL ? "" : th->hit[h]->desc);
    /* do NOT use *s with unlimited (INT_MAX) line length. Some systems
     * have an fprintf() bug here (we found one on an Opteron/SUSE Linux
     * system (#h66))
     */
  }
  if (th->nreported == 0) fprintf(ofp, "\n   [No hits detected that satisfy reporting thresholds]\n");

  if(namestr != NULL) free(namestr);
  if(posstr  != NULL) free(posstr);

  return eslOK;

 ERROR:
  if(namestr != NULL) free(namestr);
  if(posstr  != NULL) free(posstr);

  return status;
}


/* Function:  cm_tophits_HitAlignments()
 * Synopsis:  Standard output format for alignments.
 * Incept:    EPN, Tue May 24 13:50:53 2011
 *            SRE, Tue Dec  9 09:32:32 2008 [Janelia] (p7_tophits.c::p7_tophits_Domains())
 *
 * Purpose:   For each reportable target sequence, output a tabular summary
 *            of each hit followed by its alignment.
 * 
 *            Similar to <cm_tophits_Targets()>; see additional notes there.
 */
int
cm_tophits_HitAlignments(FILE *ofp, CM_TOPHITS *th, CM_PIPELINE *pli, int textw)
{
  int h;
  int namew, descw;
  char *showname;

  fprintf(ofp, "Hit alignments:\n");

  for (h = 0; h < th->N; h++) {
    if (th->hit[h]->flags & CM_HIT_IS_REPORTED) {
      if (pli->show_accessions && th->hit[h]->acc != NULL && th->hit[h]->acc[0] != '\0') {
	showname = th->hit[h]->acc;
	namew    = strlen(th->hit[h]->acc);
      }
      else {
	showname = th->hit[h]->name;
	namew = strlen(th->hit[h]->name);
      }

      if (textw > 0) {
	descw = ESL_MAX(32, textw - namew - 5);
	fprintf(ofp, ">> %s  %-.*s\n", showname, descw, (th->hit[h]->desc == NULL ? "" : th->hit[h]->desc));
      }
      else {
	fprintf(ofp, ">> %s  %s\n",    showname,        (th->hit[h]->desc == NULL ? "" : th->hit[h]->desc));
      }

      /* The hit table is 70+x char wide:, where x is the the number of digits in the total number of hits 
       *        #    score    Evalue cm from   cm to       hit from      hit to     acc
       *     ----   ------ --------- ------- -------    ----------- -----------    ----
       *        1    123.4    6.8e-9       3    1230 []           1         492 .. 0.90
       *     1234   123456 123456789 1234567 1234567 12 12345678901 12345678901 12 1234
       *     0        1         2         3         4         5         6         7
       *     12345678901234567890123456789012345678901234567890123456789012345678901234
       */

      fprintf(ofp, " %4s %1s %6s %9s %7s %7s %2s %11s %11s %2s %4s\n",  "#",    "", "score",   "Evalue",    "cm from", "cm to",   "", "hit from",    "hit to",      "", "acc");
      fprintf(ofp, " %4s %1s %6s %9s %7s %7s %2s %11s %11s %2s %4s\n",  "----", "", "-------", "---------", "-------", "-------", "", "-----------", "-----------", "", "----");
      
      fprintf(ofp, " %4d %c %6.1f %9.2g %7d %7d %c%c %11ld %11ld %c%c %4.2f\n",
	      h,
	      (th->hit[h]->flags & CM_HIT_IS_INCLUDED ? '!' : '?'),
	      th->hit[h]->score,
	      th->hit[h]->evalue,
	      th->hit[h]->ad->cfrom,
	      th->hit[h]->ad->cto,
	      (th->hit[h]->ad->cfrom == 1 ? '[' : '.'),
	      /*(th->hit[h]->ad->cto   == th->hit[h]->ad->clen ? ']' : '.'),*/
	      '?',
	      th->hit[h]->start,
	      th->hit[h]->stop,
	      (th->hit[h]->start == 1 ? '[' : '.'),
	      /*(th->hit[h]->stop == th->hit[h]->ad->L ? '[' : '.'),*/
	      '?',
	      (th->hit[h]->oasc / (1.0 + fabs((float) (th->hit[h]->stop - th->hit[h]->start)))));
    }

    if (pli->show_alignments) { 
      fprintf(ofp, "\n  Alignment:\n");
      fprintf(ofp, "  score: %.1f bits\n", th->hit[h]->score);
      /* To do, add textw capacity */
      PrintFancyAli(ofp, th->hit[h]->ad, 0, ((th->hit[h]->stop < th->hit[h]->start) ? TRUE : FALSE), FALSE);
      fprintf(ofp, "\n");
    }
    else
      fprintf(ofp, "\n");
  }
  if (th->nreported == 0) { fprintf(ofp, "\n   [No targets detected that satisfy reporting thresholds]\n"); return eslOK; }

  return eslOK;
}
/*---------------- end, standard output format ------------------*/

/*****************************************************************
 * 3. Tabular (parsable) output of pipeline results.
 *****************************************************************/

/* Function:  cm_tophits_TabularTargets()
 * Synopsis:  Output parsable table of per-sequence hits.
 * Incept:    EPN, Tue May 24 14:24:06 2011
 *            SRE, Wed Mar 18 15:26:17 2009 [Janelia] (p7_tophits.c)
 *
 * Purpose:   Output a parseable table of reportable per-sequence hits
 *            in sorted tophits list <th> in an easily parsed ASCII
 *            tabular form to stream <ofp>, using final pipeline
 *            accounting stored in <pli>.
 *            
 *            Designed to be concatenated for multiple queries and
 *            multiple top hits list.
 *
 * Returns:   <eslOK> on success.
 */
int
cm_tophits_TabularTargets(FILE *ofp, char *qname, char *qacc, CM_TOPHITS *th, CM_PIPELINE *pli, int show_header)
{
  int status;
  int i;
  int qnamew = ESL_MAX(20, strlen(qname));
  int tnamew = ESL_MAX(20, cm_tophits_GetMaxNameLength(th));
  int qaccw  = ((qacc != NULL) ? ESL_MAX(10, strlen(qacc)) : 10);
  int taccw  = ESL_MAX(10, cm_tophits_GetMaxAccessionLength(th));
  int posw   = ESL_MAX(8, cm_tophits_GetMaxPositionLength(th));

  char *qnamestr = NULL;
  char *tnamestr = NULL;
  char *qaccstr  = NULL;
  char *taccstr  = NULL;
  char *posstr   = NULL;

  ESL_ALLOC(qnamestr, sizeof(char) * (qnamew+1));
  ESL_ALLOC(tnamestr, sizeof(char) * (tnamew+1));
  ESL_ALLOC(qaccstr,  sizeof(char) * (qaccw+1));
  ESL_ALLOC(taccstr,  sizeof(char) * (taccw+1));
  ESL_ALLOC(posstr,   sizeof(char) * (posw+1));

  for(i = 0; i < qnamew; i++) { qnamestr[i] = '-'; } qnamestr[qnamew] = '\0';
  for(i = 0; i < tnamew; i++) { tnamestr[i] = '-'; } tnamestr[tnamew] = '\0';
  for(i = 0; i < qaccw;  i++) { qaccstr[i]  = '-'; } qaccstr[qaccw]   = '\0';
  for(i = 0; i < taccw;  i++) { taccstr[i]  = '-'; } taccstr[taccw]   = '\0';
  for(i = 0; i < posw;   i++) { posstr[i]   = '-'; } posstr[posw]     = '\0';

  int h;

  if (show_header) { 
    fprintf(ofp, "#%-*s %-*s %-*s %-*s %-7s %-7s %*s %*s %9s %6s %5s %s\n",
	    tnamew-1, " target name", taccw, "accession",  qnamew, "query name", qaccw, "accession", 
	    "cm from", "cm to", 
	    posw, "hit from", posw, "hit to", "E-value", "score", "strand", "description of target");
    fprintf(ofp, "#%-*s %-*s %-*s %-*s %-7s %-7s %*s %*s %9s %6s %5s %s\n",
	    tnamew-1, tnamestr, taccw, taccstr, qnamew, qnamestr, qaccw, qaccstr, 
	    "-------", "-------", 
	    posw, posstr, posw, posstr, "---------", "------", "-----", "---------------------");
  }
  for (h = 0; h < th->N; h++) { 
    if (th->hit[h]->flags & CM_HIT_IS_REPORTED)    {
      fprintf(ofp, "%-*s %-*s %-*s %-*s %7d %7d %*ld %*ld %9.2g %6.1f %6s %s\n",
	      tnamew, th->hit[h]->name,
	      taccw,  ((th->hit[h]->acc != NULL && th->hit[h]->acc[0] != '\0') ? th->hit[h]->acc : "-"),
	      qnamew, qname,
	      qaccw,  ((qacc != NULL && qacc[0] != '\0') ? qacc : "-"),
	      th->hit[h]->ad->cfrom,
	      th->hit[h]->ad->cto,
	      posw, th->hit[h]->start,
	      posw, th->hit[h]->stop,
	      th->hit[h]->evalue,
	      th->hit[h]->score,
	      (th->hit[h]->start < th->hit[h]->stop ? "   +  "  :  "   -  "),
	      (th->hit[h]->desc != NULL) ? th->hit[h]->desc : "");
    }
  }
  if(qnamestr != NULL) free(qnamestr);
  if(tnamestr != NULL) free(tnamestr);
  if(qaccstr  != NULL) free(qaccstr);
  if(qaccstr  != NULL) free(taccstr);
  if(posstr   != NULL) free(posstr);

  return eslOK;

 ERROR:
  if(qnamestr != NULL) free(qnamestr);
  if(tnamestr != NULL) free(tnamestr);
  if(qaccstr  != NULL) free(qaccstr);
  if(qaccstr  != NULL) free(taccstr);
  if(posstr   != NULL) free(posstr);

  return status;
}

/* Function:  cm_tophits_TabularTail()
 * Synopsis:  Print a trailer on a tabular output file.
 * Incept:    SRE, Tue Jan 11 16:13:30 2011 [Janelia]
 *
 * Purpose:   Print some metadata as a trailer on a tabular output file:
 *            date/time, the program, HMMER3 version info, the
 *            pipeline mode (SCAN or SEARCH), the query and target
 *            filenames, a spoof commandline recording the entire
 *            program configuration, and a "fini!" that's useful for
 *            detecting successful output completion.
 *
 * Args:      ofp       - open tabular output file (either --tblout or --domtblout)
 *            progname  - "hmmscan", for example
 *            pipemode  - CM_SEARCH_SEQS | CM_SCAN_MODELS
 *            qfile     - name of query file, or '-' for stdin, or '[none]' if NULL
 *            tfile     - name of target file, or '-' for stdin, or '[none]' if NULL
 *            go        - program configuration; used to generate spoofed command line
 *
 * Returns:   <eslOK>.
 *
 * Throws:    <eslEMEM> on allocation failure.
 *            <eslESYS> if time() or ctime_r() system calls fail.
 *                        
 * Xref:      J7/54
 */
int
cm_tophits_TabularTail(FILE *ofp, const char *progname, enum cm_pipemodes_e pipemode, const char *qfile, const char *tfile, const ESL_GETOPTS *go)
{
   time_t date           = time(NULL);
   char  *spoof_cmd      = NULL;
   char  *cwd            = NULL;
   char   timestamp[32];
   char   modestamp[16];
   int    status;

  if ((status = esl_opt_SpoofCmdline(go, &spoof_cmd)) != eslOK) goto ERROR;
  if (date == -1)                                               ESL_XEXCEPTION(eslESYS, "time() failed");
  if ((ctime_r(&date, timestamp)) == NULL)                      ESL_XEXCEPTION(eslESYS, "ctime_r() failed");
  switch (pipemode) {
  case CM_SEARCH_SEQS: strcpy(modestamp, "SEARCH"); break;
  case CM_SCAN_MODELS: strcpy(modestamp, "SCAN");   break;
  default:             ESL_EXCEPTION(eslEINCONCEIVABLE, "wait, what? no such pipemode");
  }
  esl_getcwd(&cwd);

  fprintf(ofp, "#\n");
  fprintf(ofp, "# Program:         %s\n",      (progname == NULL) ? "[none]" : progname);
  fprintf(ofp, "# Version:         %s (%s)\n", INFERNAL_VERSION, INFERNAL_DATE);
  fprintf(ofp, "# Pipeline mode:   %s\n",      modestamp);
  fprintf(ofp, "# Query file:      %s\n",      (qfile    == NULL) ? "[none]" : qfile);
  fprintf(ofp, "# Target file:     %s\n",      (tfile    == NULL) ? "[none]" : tfile);
  fprintf(ofp, "# Option settings: %s\n",      spoof_cmd);
  fprintf(ofp, "# Current dir:     %s\n",      (cwd      == NULL) ? "[unknown]" : cwd);
  fprintf(ofp, "# Date:            %s",        timestamp); /* timestamp ends in \n */
  fprintf(ofp, "# [ok]\n");

  free(spoof_cmd);
  if (cwd) free(cwd);
  return eslOK;

 ERROR:
  if (spoof_cmd) free(spoof_cmd);
  if (cwd)       free(cwd);
  return status;
}
/*------------------- end, tabular output -----------------------*/

/*****************************************************************
 * 4. Benchmark driver
 *****************************************************************/
/*****************************************************************
 * 5. Test driver
 *****************************************************************/
/*****************************************************************
 * @LICENSE@
 *****************************************************************/





