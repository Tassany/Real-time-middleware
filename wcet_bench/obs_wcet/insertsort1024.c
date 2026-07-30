/* $Id: insertsort.c,v 1.2 2005/04/04 11:34:58 csg Exp $ */

/*************************************************************************/
/*                                                                       */
/*   SNU-RT Benchmark Suite for Worst Case Timing Analysis               */
/*   =====================================================               */
/*                              Collected and Modified by S.-S. Lim      */
/*                                           sslim@archi.snu.ac.kr       */
/*                                         Real-Time Research Group      */
/*                                        Seoul National University      */
/*                                                                       */
/*                                                                       */
/*        < Features > - restrictions for our experimental environment   */
/*                                                                       */
/*          1. Completely structured.                                    */
/*               - There are no unconditional jumps.                     */
/*               - There are no exit from loop bodies.                   */
/*                 (There are no 'break' or 'return' in loop bodies)     */
/*          2. No 'switch' statements.                                   */
/*          3. No 'do..while' statements.                                */
/*          4. Expressions are restricted.                               */
/*               - There are no multiple expressions joined by 'or',     */
/*                'and' operations.                                      */
/*          5. No library calls.                                         */
/*               - All the functions needed are implemented in the       */
/*                 source file.                                          */
/*                                                                       */
/*                                                                       */
/*************************************************************************/
/*                                                                       */
/*  FILE: insertsort1024.c                                               */
/*  SOURCE : Public Domain Code                                          */
/*                                                                       */
/*  DESCRIPTION :                                                        */
/*                                                                       */
/*     Insertion sort for 1024 integer numbers.                          */
/*     The integer array a[] is initialized in main function.            */
/*                                                                       */
/*  REMARK :                                                             */
/*                                                                       */
/*     This is NOT the canonical Malardalen insertsort, which sorts 10   */
/*     elements and lives in ../insertsort.c untouched.  Li et al. state */
/*     in Section 5 that they "changed the length of the target reversed */
/*     array in insertsort to 1024", which is why insertsort dominates   */
/*     their Figures 9 and 10.  This file is that variant, so the        */
/*     comparison against Figure 10 has a row measuring the same work    */
/*     they measured.  Both binaries are built; neither replaces the     */
/*     other.                                                            */
/*                                                                       */
/*     The only changes from the original are the array length, the      */
/*     initialiser (a loop, because 1024 assignments written out would   */
/*     be unreadable) and the outer loop bound.  a[0] stays the zero     */
/*     sentinel that terminates the inner loop, and the array is filled  */
/*     fully reversed, which is insertion sort's worst case:             */
/*     sum(i-1) for i in 2..1024 = 523,776 inner iterations.             */
/*                                                                       */
/*  EXECUTION TIME :                                                     */
/*                                                                       */
/*                                                                       */
/*************************************************************************/

#define N_ELEM 1024

#ifdef DEBUG
int cnt1, cnt2;
#endif

unsigned int a[N_ELEM + 1];

int main()
{
  int  i,j, temp;

  a[0] = 0;   /* assume all data is positive */
  /* Fully reversed: a[1] = 1025, a[2] = 1024, ... a[1024] = 2.  The original
     10-element version is a[i] = 12 - i, i.e. the same descending run. */
  i = 1;
  while (i <= N_ELEM) {
    a[i] = (unsigned int)(N_ELEM + 2 - i);
    i++;
  }
  i = 2;
  while(i <= N_ELEM){
#ifdef DEBUG
      cnt1++;
#endif
      j = i;
#ifdef DEBUG
	cnt2=0;
#endif
      while (a[j] < a[j-1])
      {
#ifdef DEBUG
	cnt2++;
#endif
	temp = a[j];
	a[j] = a[j-1];
	a[j-1] = temp;
	j--;
      }
#ifdef DEBUG
	printf("Inner Loop Counts: %d\n", cnt2);
#endif
      i++;
    }
#ifdef DEBUG
    printf("Outer Loop : %d ,  Inner Loop : %d\n", cnt1, cnt2);
#endif
    return 1;
}
