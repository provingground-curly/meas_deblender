/* <AUTO>
  FILE: atGaussianWidth
<HTML>
 Code to find the optimum sigma for smoothing.
</HTML>
</AUTO>
 * 
 * jeg 960512
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#if defined(VXWORKS) /* online da computers */
#include "trace.h"
#define shAssert(ex) {if (!(ex)){TRACE(0,"atCentroid: Assertion failed: %s, line %d", #ex, __LINE__,0,0); return -1*__LINE__;}}

#elif defined (NOASTROTOOLS) /* standalone compilation; astrotools
				not available */
#include <assert.h>
#define shAssert(ex) assert(ex)
#define U16 unsigned short int
#define M_PI 3.14159265358979323846

#else                /* normal astrotools compilation */
#include "dervish.h"
#endif
#include "atGaussianWidth.h"

#define DXF  0.5
#define DYF  0.5
#define EPS  1E-10
/* 
 * These are the offsets needed to go from natural floating coordinates
 * in which an object CENTERED in pixel i j has floating coordinates
 * i.0000 j.0000 to the insane ones adopted by the SDSS, in which an
 * object centered on the LL corner of a pixel (and therefor has its light
 * equally shared with pixels i-1,j i-1,j-1 and i,j-1) has zero fractional 
 * part. These routines can be used in rational systems by setting DXF and DYF
 * to zero.
 */

#define SIZFIL 50
/* max length of filter arrays */
static short int fgarray[SIZFIL];
static short int xfgarray[SIZFIL];
static short int x2fgarray[SIZFIL];
/* arrays to hold generated gaussian and
 * moment filters
 */

static int sig_ncut=0;           /* the size of the last generated filter */
static double sigmagen = 0.;     /* the width param of the last gen. filter */

static int 
 lgausmom( const U16 **p, 
           int xsz, 
           int ysz, 
           int x,
           int y,
           int sky,
           GAUSSMOM *ps);

/* 
 * The scheme used for these focus routines uses gaussian-weighted moments
 * to calculate image sizes. If a gaussian star of width parameter sigma
 * is multiplied by another gaussian of the same width parameter times
 * the polynomial C*( 2*(r/sigma)^2 - 2) and integrated, the result is 
 * zero; this function is the first in a set of circularly symmetric
 * (gaussian * polynomial) orthogonal functions. The real stars are not,
 * of course, gaussians, but we use the same scheme; findsigma() finds the
 * value of sigma for which the star*gaussian*(1-(r/sigma)^2) integrates to 
 * zero; the resultant value of sigma is an 'equivalent' sigma the gaussian of 
 * which width in some sense 'best' represents the real psf. It can be shown
 * that that gaussian is the most efficient gaussian filter for finding
 * objects, and is so close to optimum for astrometric centering that it is
 * not worth refining. In order to eliminate the dependence on the brightness
 * of the star, we use the normalized moment
 *
 *         Sum(star*gaussian*(2*(r/sigma)^2)-2)/Sum(star*gaussian)
 *
 * to measure the deviation from focus; with the correct sigma this
 * vanishes in focus, and it grows approximately quadratically with deviations
 * from the correct focus. The scheme is thus to monitor sigma in an
 * approximately in-focus image, and monitor the focus moments in 
 * pairs of symmetrically out-of-focus images; we then use 
 * a simple quadratic interpolation scheme to find out how far we are
 * from the true focus.
 */


/************************ SETFSIGMA() *********************************/
/* This should be thought about carefully. 
 In the production code, there should be a library of these separated by 
 about 10% in sigma from about 0.7 to 2.0 or so, from which the one one 
 wants is generated by linear interpolation. Especially in the case of
 the focus servo, one is changing sigma constantly, and the interpolation
 scheme is badly needed.
*/
/* <AUTO EXTRACT>

  ROUTINE: atSetFSigma

  DESCRIPTION:
<HTML>
 This routine calculates gaussian smoothing and moment arrays for a 
 given gaussian psf width parameter sigma. The normalization is to a value
 of about 512 in the center.  The routine bombs and RETURNs (-1) if a 
 ridiculously large sigma (>12) is asked for.
 
<P>
  RETURNS:
<listing>
 	<0>	OK
 	<-1>	Requested sigma must be in the range 0<sigma<12.
</listing>
</HTML>
</AUTO>
 */ 
 
int atSetFSigma(double sigma)
{
    int ncut;
    float sig2inv,siginv;
    float edge;
    int i;
    int sum = 0;
    float isig2;
    float gau;

    if(sigma <= 0. || sigma > 12.){
        return (-1);
    }
    if(sigma == sigmagen) return 0;  /* already done it */
    sigmagen = sigma;
    
    ncut = 4.*sigma + 1.5;  /* round(4*sigma) + 1 */
    sig2inv = 0.5/(sigma*sigma);
    siginv = 1./sigma;
    edge = 512.*exp(-((double)(ncut*ncut))*sig2inv);
    sig_ncut = ncut;

    for(i=0;i<ncut;i++){
        isig2 = (double)i;
        isig2 = isig2*isig2*sig2inv;
        fgarray[i] = gau = 512.*exp(-isig2) - edge + 0.5;
        xfgarray[i] = i*gau*siginv;
        x2fgarray[i] = 2.*gau*isig2;
        sum += fgarray[i];
        if((x2fgarray[i] == 0) && i) sig_ncut = i;  /* trim to last nonzero val */
    }
    for(i=ncut;i<SIZFIL;i++) fgarray[i] = 0;  /* zero out rest of array */

    /* 
     * normalization of filter; nominally 2pi * sigma^2 * 2^18 
     */
    return 0; 
}

/********************** LGAUSMOM() ****************************************/
/*
 * This routine calculates the normalized moments of a star multiplied by
 * a gaussian of width sigmagen (last filter generated) and the polynomials
 * 2x^2 -1 and 2y^2-1. It returns their sum and, if the pointer supplied
 * is nonzero, populates the first three entries of the moment structure.
 * p is the image, xsz, ysz the x and y sizes, 
 * x and y the integer pixel locations; the moments at the
 * floating interpolated center are found later by atFindFocMom().
 * sky is an integer estimate of the background; this routine is designed
 * to work on bright objects (focus stars) so one does not need to be
 * too fussy about this, but the sky is nonnegligible in the focus
 * array, and it needs to be determined somehow.
 *
 * RETURNS:
 *	0	OK
 *	-1	Zero Gaussian integral
 */


static int
lgausmom(   const U16 **p, 
            int xsz,            /* NOTUSED */
            int ysz,            /* NOTUSED */
            int x,
            int y,
            int sky,
            GAUSSMOM *ps
            )
{
    int i,sum,xysum,x2sum,y2sum;
    register int lsum,lxsum,lx2sum;
    register int n;
    register U16 *lptrpp,*lptrpm,*lptrmp,*lptrmm;
    register int psum;
    int ncut = sig_ncut;
    float x2mom,y2mom,pmom,mmom,fsum;
    int sky2 = 2*sky;
    int sky4 = 4*sky;
    
    shAssert(sigmagen!=0);
    if(ncut+y>ysz) { printf("A ncut=%d y=%d x=%d,xsz=%d ysz=%d in lgausmom\n",ncut,y,x,xsz,ysz); return -1;}
    if(ncut>y+1)   { printf("B ncut=%d y=%d x=%d,xsz=%d ysz=%d in lgausmom\n",ncut,y,x,xsz,ysz); return -1;}

    sum = 0;
    x2sum = 0;
    y2sum = 0;
    xysum = 0;
    for(i=0; i< ncut; i++){
        lptrpm = lptrpp = (U16 *)p[y + i] + x;
        lptrmm = lptrmp = (U16 *)p[y - i] + x;
        lxsum = 0.;
        lx2sum = 0.;
        n = ncut-1;

        if(i==0){     /* do not double-count central line */
            lsum = ((*lptrpm) - sky)*fgarray[0];        
            /*debug sump += ((*lptrpm) - sky); */
            while(n){
                lsum += (psum = ((*(++lptrpp) + *(--lptrpm)) - sky2))
                            *fgarray[ncut-n];
                lxsum += ((*lptrpp) - (*lptrpm) - sky2) * xfgarray[ncut-n];
                lx2sum += psum * x2fgarray[ncut-n];
                n--;
            }
        }else{       
            lsum = ((*lptrpm) + (*lptrmm) - sky2)*fgarray[0];
            /*debug sump += ((*lptrpm) + (*lptrmm) - sky2); */
            while(n){
                psum = ((*(++lptrpp) + *(--lptrpm) + *(++lptrmp) + *(--lptrmm))
                         - sky4);
                lsum += psum * fgarray[ncut-n];

                lxsum += ((*lptrpp) - (*lptrpm) - (*lptrmp) + (*lptrmm)) * xfgarray[ncut-n];                       

                lx2sum += psum * x2fgarray[ncut-n];                       
                n--;
            }
        }


        /*debug printf("\ni,lsum,lx2sum=%d %d %d",i,lsum,lx2sum); */

        /* now for reasonable psfs, there cannot have been overflows
         * to this point, but there is no guarantee that when lsum is
         * multiplied by the filter once more that there will not be. We will
         * shift to make sure the final result is OK, but we must watch for
         * intermediate overflow.
         */

        if(lsum > 0xfffff){         /* big */
            lsum = (lsum >> 8) ;    /* shift down before you multiply */
            lxsum = (lxsum >> 8);
            lx2sum = (lx2sum >> 8);
            sum += lsum*fgarray[i];
            y2sum += lsum*x2fgarray[i];
            x2sum += lx2sum*fgarray[i];
            xysum += lxsum*xfgarray[i];
        }else{
            sum += (lsum*fgarray[i])>>8;
            y2sum += (lsum*x2fgarray[i])>>8;
            x2sum += (lx2sum*fgarray[i])>>8;
            xysum += (lxsum*xfgarray[i])>>8;
        }
        /*debug printf("\nsum,y2sum,x2sum=%d %d %d",sum,y2sum,x2sum); */
    }
    sum = sum >> 5; 
    x2sum = x2sum >> 5;
    y2sum = y2sum >> 5;
    xysum = xysum >> 5;
    /* scale things down a little just for the hell of it */

    fsum = sum;
    if (fsum==0) return -1;
    x2mom = (double)(2*x2sum - sum)/fsum;
    y2mom = (double)(2*y2sum - sum)/fsum;
    pmom = (double)(x2sum - 2*xysum + y2sum - sum)/fsum;
    mmom = (double)(x2sum + 2*xysum + y2sum - sum)/fsum;

    if(ps){
        ps->g_xmom = x2mom;
        ps->g_ymom = y2mom;
        ps->g_pmom = pmom;
        ps->g_mmom = mmom;
        ps->g_filval = fsum;
    }

    /*debug printf("\nx,y,x2mom,y2mom,filval=%d %d %f %f %f",
                x,y,x2mom,y2mom,ps->g_filval); */

    return 0;
}


/********************* FINDFOCMOM() ***************************************/
/* 
 * This routine finds the position of a focus image (to second order only)
 * and populates the proferred moment structure quadratically interpolated to 
 * the float position of the object. p is the image, xsz, ysz the x and y
 * sizes. x and y are guesses for the closest pixel to the maximum 
 * in the gaussian-smoothed image (the routine iterates if this
 * guess is not correct); ps is a pointer to a GAUSSMOM structure in
 * which the moments and position are returned, and sky is the value of the 
 * background. The routine returns the number of tries it has to make in order
 * to find the maximum pixel, but bombs and returns -1 if the number of tries
 * is larger than the threshold FINDERR or if the star is too close to the
 * edge of the frame. If the image is at all normal and of reasonably high 
 * signal-to-noise it will never require more than one iteration if the 
 * initial try is the maximum pixel in the unsmoothed image.
 *
 * RETURN:
 *	0	OK
 *	-1	Too close to edge
 *	-2	Error in lgausmom - check sky value
 */

#define FINDERR 15
        /* allowed number of tries to find max in smoothed image */
 
/* <AUTO EXTRACT>

  ROUTINE: atFindFocMom

  DESCRIPTION:
	Given the array of pixel values, the x and y sizes of the array, the
integer x and y  positions of the object, and the sky value, returns the 
interpolated center, filter value, and Gaussian weighted moments in the 
struct GAUSSMOM *ps.
<HTML>
 
<P>
  RETURNS:
<listing>
 	<0>	OK
 	<-1>	star is too close to the edge
 	<-2>	Error in lgausmom - check sky value
	<-3>	Flat peak
</listing>
</HTML>
</AUTO>
 */ 

int atFindFocMom(const U16 **p, 
           int xsz, 
           int ysz, 
           int x,
           int y,
           int sky, 
           GAUSSMOM *ps
           )
{
    int i,j;
    GAUSSMOM sq[9];
    GAUSSMOM *val[3];  
    float v,vc;
    float xvc,yvc,pvc,mvc;
    int xp,yp;     
    float dx,dy,dp,dm; 
    float d2x, d2y, d2p, d2m;
    float sx, sy, sp, sm;
    float v0;    /* value at maximum */    
    float xd2x, xd2y; 
    float xsx, xsy;
    float yd2x, yd2y;
    float ysx, ysy;
    float pd2p, pd2m; 
    float psp, psm;
    float md2p, md2m;
    float msp, msm;
    int ncut = sig_ncut;
    int ret = 0;

#ifdef NOTDEF
    /* We will bomb if the star is too close to an edge; one could do fancier
     * things, but one would regret it.
     */
    if( x < ncut+FINDERR || y < ncut+FINDERR || 
        x > xsz - ncut -1 -FINDERR || y > ysz - ncut -1 -FINDERR){
        return -1;
    }
#endif
        
    /* set up pointers */
    for(i=0;i<3;i++)  val[i] =  sq + 3*i;
    
start:

    if(ret >= FINDERR) return (-1);

    /* We will bomb if the star is too close to an edge; one could do fancier
     * things, but one would regret it.
     */
    if( (x < ncut) || (y < ncut) || 
        (x > xsz - ncut -1) || (y > ysz - ncut -1) ){
        return -1;
    }

    if (lgausmom(p,xsz,ysz,x,y,sky,&val[1][1])<0) return -2; 
                /* get central value first; then rest.*/
    vc = (val[1][1]).g_filval;
    for(i=0;i<3;i++){
        yp = y + i -1;
        for(j=0;j<3;j++){
            xp = x + j -1;
            if(!(i==1 && j==1)){
                if (lgausmom(p,xsz,ysz,xp,yp,sky,&val[i][j])<0) return -2;
                v= (val[i][j]).g_filval;
                if(v > vc){      /* 
                                  * missed the maximum--ie the max in the smth
                                  * image is not at the same place as that 
                                  * in the raw image.
                                  */
                    ret++;
                    x += j-1;
                    y += i-1;
                    goto start;  /* we could call ourselves recursively,
                                  * but this allows us to set a semi-error
                                  * flag. We could also save some time
                                  * by saving and moving the values we have 
                                  * already computed, but the bookkeeping is
                                  * messy.
                                  */
                }
            }
        }
    }

    
    d2x = 2.*vc - val[1][2].g_filval - val[1][0].g_filval;
    if (d2x < EPS) {return -3;}
    sx  = 0.5*(val[1][2].g_filval - val[1][0].g_filval);
    dx  = sx/d2x;
    
    d2y = 2.*vc - val[2][1].g_filval - val[0][1].g_filval;
    if (d2y < EPS) {return -3;}
    sy  = 0.5*(val[2][1].g_filval - val[0][1].g_filval);
    dy  = sy/d2y;
    
    d2p = 2.*vc - val[2][2].g_filval - val[0][0].g_filval;
    if (d2p < EPS) {return -3;}
    sp  = 0.5*(val[2][2].g_filval - val[0][0].g_filval);
    dp  = sp/d2p;
    
    d2m = 2.*vc - val[2][0].g_filval - val[0][2].g_filval;
    if (d2m < EPS) {return -3;}
    sm  = 0.5*(val[2][0].g_filval - val[0][2].g_filval);
    dm  = sm/d2m;
    
    ps->g_xf = x + dx + DXF;
    ps->g_yf = y + dy + DYF;

    /* these are just the parabolic values for the central crossstripe */

    v0 = vc + 0.5*(((sx*sx)/d2x + (sy*sy)/d2y));
    ps->g_filval = v0/32.;
    /* 
     * Estimate for real peak value above sky.
     */

    /* now interpolate the moments */
    xvc = val[1][1].g_xmom; 
    yvc = val[1][1].g_ymom; /* central pixel moment values */
    pvc = val[1][1].g_pmom; 
    mvc = val[1][1].g_mmom; 

    xd2x = 2.*xvc - val[1][2].g_xmom - val[1][0].g_xmom;
    xd2y = 2.*xvc - val[2][1].g_xmom - val[0][1].g_xmom;
    xsx  = 0.5*(val[1][2].g_xmom - val[1][0].g_xmom);
    xsy  = 0.5*(val[2][1].g_xmom - val[0][1].g_xmom);
    
    yd2x = 2.*yvc - val[1][2].g_ymom - val[1][0].g_ymom;   
    yd2y = 2.*yvc - val[2][1].g_ymom - val[0][1].g_ymom;    
    ysx  = 0.5*(val[1][2].g_ymom - val[1][0].g_ymom);
    ysy  = 0.5*(val[2][1].g_ymom - val[0][1].g_ymom);
    
    pd2p = 2.*pvc - val[2][2].g_pmom - val[0][0].g_pmom;   
    pd2m = 2.*pvc - val[2][0].g_pmom - val[0][2].g_pmom;    
    psp  = 0.5*(val[2][2].g_pmom - val[0][0].g_pmom);
    psm  = 0.5*(val[2][0].g_pmom - val[0][2].g_pmom);
    
    md2p = 2.*mvc - val[2][2].g_mmom - val[0][0].g_mmom;   
    md2m = 2.*mvc - val[2][0].g_mmom - val[0][2].g_mmom;    
    msp  = 0.5*(val[2][2].g_mmom - val[0][0].g_mmom);
    msm  = 0.5*(val[2][0].g_mmom - val[0][2].g_mmom);
    
    ps->g_xmom = xvc + xsx*dx + xsy*dy - 0.5*(dx*dx*xd2x + dy*dy*xd2y);
    ps->g_ymom = yvc + ysx*dx + ysy*dy - 0.5*(dx*dx*yd2x + dy*dy*yd2y);
    ps->g_pmom = pvc + psp*dp + psm*dm - 0.5*(dp*dp*pd2p + dm*dm*pd2m);
    ps->g_mmom = mvc + msp*dp + msm*dm - 0.5*(dp*dp*md2p + dm*dm*md2m);
    
    return ret; 
}    

/******************** FINDSIGMA() ***************************************/

#define SALPHA 0.5  
/* acceleration parameter; this is near the optimum choice */
#define SIGGUESS 1.2
/* initial guess for width parameter; this is near the nominal value,
 * which is about 1.2 times the width parameter for the little gaussian,
 * which is near 1.0 for the nominal imaging conditions.
 */
 
#define SIGITERAT 10
/* iteration limit */
#define SIGERR 1.e-2
/* tolerance */

/* <AUTO EXTRACT>

  ROUTINE: atSigmaFind

  DESCRIPTION:
<HTML>
  this routine iterates to find the optimum sigma for smoothing for object
  finding (and, though not QUITE optimum for astrometry, is trivially
  different from the correct value and I suggest we use it) by finding the 
  sigma value for which the focus moment 2*r^2 - 2 weighted by that gaussian 
  vanishes. It returns the found value or -1.0 if any invocation of 
  atFindFocMom fails. It will populate a struct GAUSSMOM if one is offered, 
  or will use an internal one for its calculations if one is not 
  (the argument is NULL).
 
<P>
  RETURNS:
<listing>
 	<0>	OK
 	<-1>	Error in atSetFSigma (sigma too large)
 	<-2>	sigma out of range (inf sharp or inf flat)
 	<-3>	Too many iterations
 	<-4>	Too close to edge
 	<-5>	Error in lgausmom - check sky value
	<-6>	Too flat, from atFindFocMom
</listing>
</HTML>
</AUTO>
 */ 

int
atSigmaFind(const U16 **p, 
        int xsz, 
        int ysz, 
        int x,
        int y,
        int sky, 
        GAUSSMOM *ps,
	double *sig)
{
    float sigold;
    float mom;
    int i;
    int err;
    GAUSSMOM gmom;

    if (*sig<0) *sig = SIGGUESS;
    
    if(ps == NULL) ps = &gmom;
   
    for(i=0;i<SIGITERAT;i++){
        if (atSetFSigma(*sig)<0) return -1;        
        sigold = *sig;
        err = atFindFocMom(p,xsz,ysz,x,y,sky,ps);
        if(err < 0) return (err-3);
        mom = ps->g_xmom + ps->g_ymom;
	/*printf("i %d, sig %g mom %g\n", i, *sig, mom);*/
	if(ps->g_xmom<=-1.|| ps->g_xmom>=1.) return -2;
	if(ps->g_ymom<=-1.|| ps->g_ymom>=1.) return -2;
	if(ps->g_pmom<=-1.|| ps->g_pmom>=1.) return -2;
	if(ps->g_mmom<=-1.|| ps->g_mmom>=1.) return -2;
        if(fabs(mom) < SIGERR) break;
        /**sig *= (1. + SALPHA*(mom));*/
        *sig *= sqrt((2.+mom)/(2-mom));
        if(2.*fabs(sigold-*sig) < SIGERR) break;
    }
    if (i==SIGITERAT) return -3;
    return 0;
}
