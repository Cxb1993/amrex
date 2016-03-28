#include <winstd.H>

#include <algorithm>
#include <iomanip>
#include <cmath>

#ifdef BL_Darwin
using std::isinf;
#endif

#include <ParmParse.H>
#include <ParallelDescriptor.H>
#include <Utility.H>
#include <LO_BCTYPES.H>
#include <CG_F.H>
#include <CGSolver.H>
#include <MultiGrid.H>
#include <VisMF.H>
//
// The largest value allowed for SSS - the "S" in the Communicaton-avoiding BiCGStab.
//
static const int SSS_MAX = 4;

namespace
{
    //
    // Set default values for these in Initialize()!!!
    //
    int  SSS;
    bool variable_SSS;
    //
    // Has Initialized() been called?
    //
    bool initialized = false;
}
//
// Set default values for these in Initialize()!!!
//
int              CGSolver::def_maxiter;
int              CGSolver::def_verbose;
CGSolver::Solver CGSolver::def_cg_solver;
bool             CGSolver::use_jbb_precond;
bool             CGSolver::use_jacobi_precond;
double           CGSolver::def_unstable_criterion;

void
CGSolver::Initialize ()
{
    if (initialized) return;
    //
    // Set defaults here!!!
    //
    SSS                              = SSS_MAX;
    variable_SSS                     = true;
    CGSolver::def_maxiter            = 80;
    CGSolver::def_verbose            = 0;
    CGSolver::def_cg_solver          = BiCGStab;
    CGSolver::use_jbb_precond        = 0;
    CGSolver::use_jacobi_precond     = 0;
    CGSolver::def_unstable_criterion = 10;

    ParmParse pp("cg");

    pp.query("v",                  def_verbose);
    pp.query("SSS",                SSS);
    pp.query("maxiter",            def_maxiter);
    pp.query("verbose",            def_verbose);
    pp.query("variable_SSS",       variable_SSS);
    pp.query("use_jbb_precond",    use_jbb_precond);
    pp.query("use_jacobi_precond", use_jacobi_precond);
    pp.query("unstable_criterion", def_unstable_criterion);

    if (SSS < 1      ) BoxLib::Abort("SSS must be >= 1");
    if (SSS > SSS_MAX) BoxLib::Abort("SSS must be <= SSS_MAX");

    int ii;
    if (pp.query("cg_solver", ii))
    {
        switch (ii)
        {
        case 0: def_cg_solver = CG;             break;
        case 1: def_cg_solver = BiCGStab;       break;
        case 2: def_cg_solver = CABiCGStab;     break;
        case 3: def_cg_solver = CABiCGStabQuad; break;
        default:
            BoxLib::Error("CGSolver::Initialize(): bad cg_solver");
        }
    }

    if ( def_verbose > 2 && ParallelDescriptor::IOProcessor() )
    {
        std::cout << "CGSolver settings ...\n";
	std::cout << "   def_maxiter            = " << def_maxiter            << '\n';
	std::cout << "   def_unstable_criterion = " << def_unstable_criterion << '\n';
	std::cout << "   def_cg_solver          = " << def_cg_solver          << '\n';
	std::cout << "   use_jbb_precond        = " << use_jbb_precond        << '\n';
	std::cout << "   use_jacobi_precond     = " << use_jacobi_precond     << '\n';
	std::cout << "   SSS                    = " << SSS                    << '\n';
    }

    BoxLib::ExecOnFinalize(CGSolver::Finalize);
    
    initialized = true;
}

void
CGSolver::Finalize ()
{
    ;
}

CGSolver::CGSolver (LinOp& _lp,
                    bool   _use_mg_precond,
                    int    _lev)
    :
    Lp(_lp),
    mg_precond(0),
    lev(_lev),
    use_mg_precond(_use_mg_precond)
{
    Initialize();
    maxiter = def_maxiter;
    verbose = def_verbose;
    set_mg_precond();
}

void
CGSolver::set_mg_precond ()
{
    delete mg_precond;
    if (use_mg_precond)
    {
        mg_precond = new MultiGrid(Lp);
    }
}

CGSolver::~CGSolver ()
{
    delete mg_precond;
}

static
void
Spacer (std::ostream& os, int lev)
{
    for (int k = 0; k < lev; k++)
    {
        os << "   ";
    }
}

static
Real
norm_inf (const MultiFab& res, bool local = false)
{
    Real restot = res.norm0(0,true);
    if ( !local )
        ParallelDescriptor::ReduceRealMax(restot);
    return restot;
}

int
CGSolver::solve (MultiFab&       sol,
                 const MultiFab& rhs,
                 Real            eps_rel,
                 Real            eps_abs,
                 LinOp::BC_Mode  bc_mode)
{
    switch (def_cg_solver)
    {
    case CG:
        return solve_cg(sol, rhs, eps_rel, eps_abs, bc_mode);
    case BiCGStab:
        return solve_bicgstab(sol, rhs, eps_rel, eps_abs, bc_mode);
    case CABiCGStab:
        return solve_cabicgstab(sol, rhs, eps_rel, eps_abs, bc_mode);
#ifdef XBLAS
    case CABiCGStabQuad:
        return solve_cabicgstab_quad(sol, rhs, eps_rel, eps_abs, bc_mode);
#endif
    default:
        BoxLib::Error("CGSolver::solve(): unknown solver");
    }

    return -1;
}

static
void
sxay (MultiFab&       ss,
      const MultiFab& xx,
      Real            a,
      const MultiFab& yy,
      int             yycomp)
{
    BL_PROFILE("CGSolver::sxay()");

    BL_ASSERT(yy.nComp() > yycomp);

    const int ncomp  = 1;
    const int sscomp = 0;
    const int xxcomp = 0;

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(ss,true); mfi.isValid(); ++mfi)
    {
        const Box&       bx    = mfi.tilebox();
        FArrayBox&       ssfab = ss[mfi];
        const FArrayBox& xxfab = xx[mfi];
        const FArrayBox& yyfab = yy[mfi];

        FORT_CGSXAY(ssfab.dataPtr(sscomp),
                    ARLIM(ssfab.loVect()), ARLIM(ssfab.hiVect()),
                    xxfab.dataPtr(xxcomp),
                    ARLIM(xxfab.loVect()), ARLIM(xxfab.hiVect()),
                    &a,
                    yyfab.dataPtr(yycomp),
                    ARLIM(yyfab.loVect()), ARLIM(yyfab.hiVect()),
                    bx.loVect(), bx.hiVect(),
                    &ncomp);
    }
}

inline
void
sxay (MultiFab&       ss,
      const MultiFab& xx,
      Real            a,
      const MultiFab& yy)
{
    sxay(ss,xx,a,yy,0);
}

//
// Do a one-component dot product of r & z using supplied components.
//
static
Real
dotxy (const MultiFab& r,
       int             rcomp,
       const MultiFab& z,
       int             zcomp,
       bool            local)
{
    BL_PROFILE("CGSolver::dotxy()");

    BL_ASSERT(r.nComp() > rcomp);
    BL_ASSERT(z.nComp() > zcomp);
    BL_ASSERT(r.boxArray() == z.boxArray());

    const int ncomp = 1;

    Real dot = 0;

    for (MFIter mfi(r); mfi.isValid(); ++mfi)
    {
        const Box&       rbx  = mfi.validbox();
        const FArrayBox& rfab = r[mfi];
        const FArrayBox& zfab = z[mfi];

        Real tdot;

        FORT_CGXDOTY(&tdot,
                     zfab.dataPtr(zcomp),
                     ARLIM(zfab.loVect()),ARLIM(zfab.hiVect()),
                     rfab.dataPtr(rcomp),
                     ARLIM(rfab.loVect()),ARLIM(rfab.hiVect()),
                     rbx.loVect(),rbx.hiVect(),
                     &ncomp);
        dot += tdot;
    }

    if ( !local )
        ParallelDescriptor::ReduceRealSum(dot);

    return dot;
}

inline
Real
dotxy (const MultiFab& r,
       const MultiFab& z,
       bool            local = false)
{
    return dotxy(r,0,z,0,local);
}

//
// z[m] = A[m][n]*x[n]   [row][col]
//
inline
void
gemv (Real*       z,
      Real        A[((4*SSS_MAX)+1)][((4*SSS_MAX)+1)],
      const Real* x,
      int         rows,
      int         cols)
{
    for (int r = 0; r < rows; r++)
    {
        Real sum = 0;
        for (int c = 0; c < cols; c++)
        {
            sum += A[r][c]*x[c];
        }
        z[r] = sum;
    }
}

//
// z[n] = x[n]+beta*y[n]
//
inline
void
axpy (Real*       z,
      const Real* x,
      Real        beta,
      const Real* y,
      int         n)
{
    for (int nn = 0; nn < n; nn++)
    {
        z[nn] = x[nn] + beta*y[nn];
    }
}

//
// x[n].y[n]
//
inline
Real
dot (const Real* x,
     const Real* y,
     int         n)
{
    Real sum = 0;
    for (int nn = 0; nn < n; nn++)
    {
        sum += x[nn]*y[nn];
    }
    return sum;
}

//
// z[n] = 0
//
inline
void
zero (Real* z, int n)
{
    for (int nn = 0; nn < n;nn++)
    {
        z[nn] = 0;
    }
}

static
void
SetMonomialBasis (Real  Tp[((4*SSS_MAX)+1)][((4*SSS_MAX)+1)],
                  Real Tpp[((4*SSS_MAX)+1)][((4*SSS_MAX)+1)],
                  int   sss)
{
    for (int i = 0; i < 4*sss+1; i++)
    {
        for (int j = 0; j < 4*sss+1; j++)
        {
            Tp[i][j] = 0;
        }
    }
    for (int i = 0; i < 2*sss; i++)
    {
        Tp[i+1][i] = 1;
    }
    for (int i = 2*sss+1; i < 4*sss; i++)
    {
        Tp[i+1][i] = 1;
    }

    for (int i = 0; i < 4*sss+1; i++)
    {
        for (int j = 0; j < 4*sss+1; j++)
        {
            Tpp[i][j] = 0;
        }
    }
    for (int i = 0; i < 2*sss-1; i++)
    {
        Tpp[i+2][i] = 1;
    }
    for (int i = 2*sss+1; i < 4*sss-1; i++)
    {
        Tpp[i+2][i] = 1;
    }
}

//
// Forward declaration of BuildGramMatrix().
// It's now later in this file so it can use some XBLAS stuff.
//
static void BuildGramMatrix (Real* Gg, const MultiFab& PR, const MultiFab& rt, int sss);

//
// Based on Erin Carson/Jim Demmel/Nick Knight's s-Step BiCGStab Algorithm 3.4.
//
// As originally implemented by:
//
// Samuel Williams
// SWWilliams@lbl.gov
// Lawrence Berkeley National Lab
//
// NOTE: If you wish to compare CABiCGStab -vs- BiCGStab make sure to compile
// this code with CG_USE_OLD_CONVERGENCE_CRITERIA defined.  The BiCGStab code
// has two different convergence criteria it can use; the CABiCGStab code is
// hard-coded to use only one convergence criterion.  They won't compare identically,
// even in that case, since the CA algorithm uses L2 norms while the non-CA
// algorithm uses the inf norm.  But they're usually pretty close in iteration
// counts.
//

int
CGSolver::solve_cabicgstab (MultiFab&       sol,
                            const MultiFab& rhs,
                            Real            eps_rel,
                            Real            eps_abs,
                            LinOp::BC_Mode  bc_mode)
{
    BL_PROFILE("CGSolver::solve_cabicgstab()");

    BL_ASSERT(sol.nComp() == 1);
    BL_ASSERT(sol.boxArray() == Lp.boxArray(lev));
    BL_ASSERT(rhs.boxArray() == Lp.boxArray(lev));

    Real  temp1[4*SSS_MAX+1];
    Real  temp2[4*SSS_MAX+1];
    Real  temp3[4*SSS_MAX+1];
    Real     Tp[4*SSS_MAX+1][4*SSS_MAX+1];
    Real    Tpp[4*SSS_MAX+1][4*SSS_MAX+1];
    Real     aj[4*SSS_MAX+1];
    Real     cj[4*SSS_MAX+1];
    Real     ej[4*SSS_MAX+1];
    Real   Tpaj[4*SSS_MAX+1];
    Real   Tpcj[4*SSS_MAX+1];
    Real  Tppaj[4*SSS_MAX+1];
    Real      G[4*SSS_MAX+1][4*SSS_MAX+1];    // Extracted from first 4*SSS+1 columns of Gg[][].  indexed as [row][col]
    Real      g[4*SSS_MAX+1];                 // Extracted from last [4*SSS+1] column of Gg[][].
    Real     Gg[(4*SSS_MAX+1)*(4*SSS_MAX+2)]; // Buffer to hold the Gram-like matrix produced by matmul().  indexed as [row*(4*SSS+2) + col]
    //
    // If variable_SSS we "telescope" SSS.
    // We start with 1 and increase it up to SSS_MAX on the outer iterations.
    //
    if (variable_SSS) SSS = 1;

    zero(   aj, 4*SSS_MAX+1);
    zero(   cj, 4*SSS_MAX+1);
    zero(   ej, 4*SSS_MAX+1);
    zero( Tpaj, 4*SSS_MAX+1);
    zero( Tpcj, 4*SSS_MAX+1);
    zero(Tppaj, 4*SSS_MAX+1);
    zero(temp1, 4*SSS_MAX+1);
    zero(temp2, 4*SSS_MAX+1);
    zero(temp3, 4*SSS_MAX+1);

    SetMonomialBasis(Tp,Tpp,SSS);

    const int ncomp = 1, nghost = sol.nGrow();
    //
    // Contains the matrix powers of p[] and r[].
    //
    // First 2*SSS+1 components are powers of p[].
    // Next  2*SSS   components are powers of r[].
    //
    MultiFab PR(sol.boxArray(), 4*SSS_MAX+1, 0);

    MultiFab  p(sol.boxArray(), ncomp, 0);
    MultiFab  r(sol.boxArray(), ncomp, 0);
    MultiFab rt(sol.boxArray(), ncomp, 0);
    
    MultiFab tmp(sol.boxArray(), 4, nghost);

    Lp.residual(r, rhs, sol, lev, bc_mode);

    if ((verbose > 1) && r.contains_nan()) std::cout << "*** r contains NANs\n";

    MultiFab::Copy(rt,r,0,0,1,0);
    MultiFab::Copy( p,r,0,0,1,0);

    const Real           rnorm0        = norm_inf(r);
    Real                 delta         = dotxy(r,rt);
    const Real           L2_norm_of_rt = sqrt(delta);
    const LinOp::BC_Mode temp_bc_mode  = LinOp::Homogeneous_BC;

    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev);
        std::cout << "CGSolver_CABiCGStab: Initial error (error0) =        " << rnorm0 << '\n';
    }

    if ( rnorm0 == 0 || delta == 0 || rnorm0 < eps_abs )
    {
        if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
	{
            Spacer(std::cout, lev);
            std::cout << "CGSolver_CABiCGStab: niter = 0,"
                      << ", rnorm = "   << rnorm0
                      << ", delta = "   << delta
                      << ", eps_abs = " << eps_abs << '\n';
	}
        return 0;
    }

    int niters = 0, ret = 0;

    Real L2_norm_of_resid = 0, atime = 0, gtime = 0;

    bool BiCGStabFailed = false, BiCGStabConverged = false;

    for (int m = 0; m < maxiter && !BiCGStabFailed && !BiCGStabConverged; )
    {
        const Real time1 = ParallelDescriptor::second();
        //
        // Compute the matrix powers on p[] & r[] (monomial basis).
        // The 2*SSS+1 powers of p[] followed by the 2*SSS powers of r[].
        //
        MultiFab::Copy(PR,p,0,0,1,0);
        MultiFab::Copy(PR,r,0,2*SSS+1,1,0);

        if ((verbose > 1) && PR.contains_nan(0,      1)) std::cout << "*** PR contains NANs @ p\n";
        if ((verbose > 1) && PR.contains_nan(2*SSS+1,1)) std::cout << "*** PR contains NANs @ r\n";
        //
        // We use "tmp" to minimize the number of Lp.apply()s.
        // We do this by doing p & r together in a single call.
        //
        MultiFab::Copy(tmp,p,0,0,1,0);
        MultiFab::Copy(tmp,r,0,1,1,0);

        for (int n = 1; n < 2*SSS; n++)
        {
            Lp.apply(tmp, tmp, lev, temp_bc_mode, false, 0, 2, 2);

            MultiFab::Copy(tmp,tmp,2,0,2,0);

            MultiFab::Copy(PR,tmp,0,        n,1,0);
            MultiFab::Copy(PR,tmp,1,2*SSS+n+1,1,0);

            if ((verbose > 1) && PR.contains_nan(n,        1)) std::cout << "*** PR contains NANs @ p: " << n         << '\n';
            if ((verbose > 1) && PR.contains_nan(2*SSS+n+1,1)) std::cout << "*** PR contains NANs @ r: " << 2*SSS+n+1 << '\n';
        }

        MultiFab::Copy(tmp,PR,2*SSS-1,0,1,0);
        Lp.apply(tmp, tmp, lev, temp_bc_mode, false, 0, 1, 1);
        MultiFab::Copy(PR,tmp,1,2*SSS,1,0);

        if ((verbose > 1) && PR.contains_nan(2*SSS-1,1)) std::cout << "*** PR contains NANs @ 2*SSS-1\n";
        if ((verbose > 1) && PR.contains_nan(2*SSS,  1)) std::cout << "*** PR contains NANs @ 2*SSS\n";

        Real time2 = ParallelDescriptor::second();

        atime += (time2-time1);

        BuildGramMatrix(Gg, PR, rt, SSS);

        const Real time3 = ParallelDescriptor::second();

        gtime += (time3-time2);
        //
        // Form G[][] and g[] from Gg.
        //
        for (int i = 0, k = 0; i < 4*SSS+1; i++)
        {
            for (int j = 0; j < 4*SSS+1; j++)
                //
                // First 4*SSS+1 elements in each row go to G[][].
                //
                G[i][j] = Gg[k++];
            //
            // Last element in row goes to g[].
            //
            g[i] = Gg[k++];
        }

        zero(aj, 4*SSS+1); aj[0]       = 1;
        zero(cj, 4*SSS+1); cj[2*SSS+1] = 1;
        zero(ej, 4*SSS+1);

        for (int nit = 0; nit < SSS; nit++)
        {
            gemv( Tpaj,  Tp, aj, 4*SSS+1, 4*SSS+1);
            gemv( Tpcj,  Tp, cj, 4*SSS+1, 4*SSS+1);
            gemv(Tppaj, Tpp, aj, 4*SSS+1, 4*SSS+1);

            const Real g_dot_Tpaj = dot(g, Tpaj, 4*SSS+1);

            if ( g_dot_Tpaj == 0 )
            {
                if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: g_dot_Tpaj == 0, nit = " << nit << '\n';
                BiCGStabFailed = true; ret = 1; break;
            }

            const Real alpha = delta / g_dot_Tpaj;

            if ( isinf(alpha) )
            {
                if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: alpha == inf, nit = " << nit << '\n';
                BiCGStabFailed = true; ret = 2; break;
            }

            axpy(temp1, Tpcj, -alpha, Tppaj, 4*SSS+1);

            gemv(temp2, G, temp1, 4*SSS+1, 4*SSS+1);

            axpy(temp3,   cj, -alpha,  Tpaj, 4*SSS+1);

            const Real omega_numerator   = dot(temp3, temp2, 4*SSS+1);
            const Real omega_denominator = dot(temp1, temp2, 4*SSS+1);
            //
            // NOTE: omega_numerator/omega_denominator can be 0/x or 0/0, but should never be x/0.
            //
            // If omega_numerator==0, and ||s||==0, then convergence, x=x+alpha*aj.
            // If omega_numerator==0, and ||s||!=0, then stabilization breakdown.
            //
            // Partial update of ej must happen before the check on omega to ensure forward progress !!!
            //
            axpy(ej, ej, alpha, aj, 4*SSS+1);
            //
            // ej has been updated so consider that we've done an iteration since
            // even if we break out of the loop we'll be able to update both sol.
            //
            niters++;
            //
            // Calculate the norm of Saad's vector 's' to check intra s-step convergence.
            //
            axpy(temp1, cj,-alpha,  Tpaj, 4*SSS+1);

            gemv(temp2, G, temp1, 4*SSS+1, 4*SSS+1);

            const Real L2_norm_of_s = dot(temp1,temp2,4*SSS+1);

            L2_norm_of_resid = (L2_norm_of_s < 0 ? 0 : sqrt(L2_norm_of_s));

            if ( L2_norm_of_resid < eps_rel*L2_norm_of_rt )
            {
                if ( verbose > 1 && L2_norm_of_resid == 0 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: L2 norm of s: " << L2_norm_of_s << '\n';
                BiCGStabConverged = true; break;
            }

            if ( omega_denominator == 0 )
            {
                if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: omega_denominator == 0, nit = " << nit << '\n';
                BiCGStabFailed = true; ret = 3; break;
            }

            const Real omega = omega_numerator / omega_denominator;

            if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
            {
                if ( omega == 0   ) std::cout << "CGSolver_CABiCGStab: omega == 0, nit = " << nit << '\n';
                if ( isinf(omega) ) std::cout << "CGSolver_CABiCGStab: omega == inf, nit = " << nit << '\n';
            }

            if ( omega == 0   ) { BiCGStabFailed = true; ret = 4; break; }
            if ( isinf(omega) ) { BiCGStabFailed = true; ret = 4; break; }
            //
            // Complete the update of ej & cj now that omega is known to be ok.
            //
            axpy(ej, ej,       omega,    cj, 4*SSS+1);
            axpy(ej, ej,-omega*alpha,  Tpaj, 4*SSS+1);
            axpy(cj, cj,      -omega,  Tpcj, 4*SSS+1);
            axpy(cj, cj,      -alpha,  Tpaj, 4*SSS+1);
            axpy(cj, cj, omega*alpha, Tppaj, 4*SSS+1);
            //
            // Do an early check of the residual to determine convergence.
            //
            gemv(temp1, G, cj, 4*SSS+1, 4*SSS+1);
            //
            // sqrt( (cj,Gcj) ) == L2 norm of the intermediate residual in exact arithmetic.
            // However, finite precision can lead to the norm^2 being < 0 (Jim Demmel).
            // If cj_dot_Gcj < 0 we flush to zero and consider ourselves converged.
            //
            const Real L2_norm_of_r = dot(cj, temp1, 4*SSS+1);

            L2_norm_of_resid = (L2_norm_of_r > 0 ? sqrt(L2_norm_of_r) : 0);

            if ( L2_norm_of_resid < eps_rel*L2_norm_of_rt )
            {
                if ( verbose > 1 && L2_norm_of_resid == 0 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: L2_norm_of_r: " << L2_norm_of_r << '\n';
                BiCGStabConverged = true; break;
            }

            const Real delta_next = dot(g, cj, 4*SSS+1);

            if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
            {
                if ( delta_next == 0   ) std::cout << "CGSolver_CABiCGStab: delta == 0, nit = " << nit << '\n';
                if ( isinf(delta_next) ) std::cout << "CGSolver_CABiCGStab: delta == inf, nit = " << nit << '\n';
            }

            if ( isinf(delta_next) ) { BiCGStabFailed = true; ret = 5; break; } // delta = inf?
            if ( delta_next  == 0  ) { BiCGStabFailed = true; ret = 5; break; } // Lanczos breakdown...

            const Real beta = (delta_next/delta)*(alpha/omega);

            if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
            {
                if ( beta == 0   ) std::cout << "CGSolver_CABiCGStab: beta == 0, nit = " << nit << '\n';
                if ( isinf(beta) ) std::cout << "CGSolver_CABiCGStab: beta == inf, nit = " << nit << '\n';
            }

            if ( isinf(beta) ) { BiCGStabFailed = true; ret = 6; break; } // beta = inf?
            if ( beta == 0   ) { BiCGStabFailed = true; ret = 6; break; } // beta = 0?  can't make further progress(?)

            axpy(aj, cj,        beta,   aj, 4*SSS+1);
            axpy(aj, aj, -omega*beta, Tpaj, 4*SSS+1);

            delta = delta_next;
        }
        //
        // Update iterates.
        //
        for (int i = 0; i < 4*SSS+1; i++)
            sxay(sol,sol,ej[i],PR,i);

        MultiFab::Copy(p,PR,0,0,1,0);
        p.mult(aj[0],0,1);
        for (int i = 1; i < 4*SSS+1; i++)
            sxay(p,p,aj[i],PR,i);

        MultiFab::Copy(r,PR,0,0,1,0);
        r.mult(cj[0],0,1);
        for (int i = 1; i < 4*SSS+1; i++)
            sxay(r,r,cj[i],PR,i);

        if ( !BiCGStabFailed && !BiCGStabConverged )
        {
            m += SSS;

            if ( variable_SSS && SSS < SSS_MAX ) { SSS++; SetMonomialBasis(Tp,Tpp,SSS); }
        }
    }

    if ( verbose > 0 )
    {
        if ( ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev);
            std::cout << "CGSolver_CABiCGStab: Final: Iteration "
                      << std::setw(4) << niters
                      << " rel. err. "
                      << L2_norm_of_resid << '\n';
        }

        if ( verbose > 1 )
        {
            Real tmp[2] = { atime, gtime };

            ParallelDescriptor::ReduceRealMax(tmp,2,ParallelDescriptor::IOProcessorNumber());

            if ( ParallelDescriptor::IOProcessor() )
            {
                Spacer(std::cout, lev);
                std::cout << "CGSolver_CABiCGStab apply time: " << tmp[0] << ", gram time: " << tmp[1] << '\n';
            }
        }
    }

    if ( niters >= maxiter && !BiCGStabFailed && !BiCGStabConverged)
    {
        if ( L2_norm_of_resid > L2_norm_of_rt )
        {
            if ( ParallelDescriptor::IOProcessor() )
                BoxLib::Warning("CGSolver_CABiCGStab: failed to converge!");
            //
            // Return code 8 tells the MultiGrid driver to zero out the solution!
            //
            ret = 8;
        }
        else
        {
            //
            // Return codes 1-7 tells the MultiGrid driver to smooth the solution!
            //
            ret = 7;
        }
    }

    return ret;
}

#ifdef XBLAS

#include <blas_enum.h>

extern "C"
{
#include <blas_extended_proto.h>
}

inline
Real
qdot (const Real* x,
      const Real* y,
      int         n)
{
    Real sum = 0;

    BLAS_ddot_x(blas_no_conj, n, 1.0, x, 1, 0.0, y, 1, &sum, blas_prec_extra);

    return sum;
}

//
// z[m] = A[m][n]*x[n]   [row][col]
//
static
void
qgemv (Real*       z,
       Real        A[((4*SSS_MAX)+1)][((4*SSS_MAX)+1)],
       const Real* x,
       int         rows,
       int         cols)
{
    Real tmp[(4*SSS_MAX+1)*(4*SSS_MAX+1)];

    for (int r = 0; r < rows; r++)
    {
        for (int c = 0; c < cols; c++)
        {
            tmp[(r*rows)+c] = A[r][c];
        }
    }

    BLAS_dgemv_x(blas_rowmajor, blas_no_trans,
                 rows, cols, 1.0, tmp, rows,
                 x, 1, 0.0, z, 1, blas_prec_extra);
}

//
// Do a one-component dot product of r & z using supplied components.
//

static
Real
qdotxy (const MultiFab& r,
        int             rcomp,
        const MultiFab& z,
        int             zcomp,
        bool            local)
{
    BL_ASSERT(r.nComp() > rcomp);
    BL_ASSERT(z.nComp() > zcomp);
    BL_ASSERT(r.boxArray() == z.boxArray());

    const int N = r.local_size();

    Array<Real> ldots(N);

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
	FArrayBox tmp;

	for (MFIter mfi(r); mfi.isValid(); ++mfi)
	{
	    const Box& bx = mfi.validbox();
	    tmp.resize(bx,2);

	    tmp.copy(r[mfi], rcomp, 0, 1);
	    tmp.copy(z[mfi], zcomp, 1, 1);
	    
	    const int NumPts = tmp.box().numPts();

	    ldots[mfi.LocalIndex()] = qdot(tmp.dataPtr(0), tmp.dataPtr(1), NumPts);
	}
    }

    Real dot = 0;

    if ( N > 0 )
    {
        dot = ldots[0];

        if ( N > 1 )
            BLAS_dsum_x(N, ldots.dataPtr(), 1, &dot, blas_prec_extra);
    }

    const int NProcs = ParallelDescriptor::NProcs();
    const int IOProc = ParallelDescriptor::IOProcessorNumber();

    if ( !local && NProcs > 1 )
    {
        //
        // Sum all "dot"s using quad sums on I/O Processor.
        //
        Array<Real> gdots(NProcs,0);

        ParallelDescriptor::Gather(&dot, 1, gdots.dataPtr(), IOProc);

        if ( ParallelDescriptor::IOProcessor() )
            //
            // Reduce'm in quad precision.
            //
            BLAS_dsum_x(NProcs, gdots.dataPtr(), 1, &dot, blas_prec_extra);
        //
        // Now broadcast the result back to the other processors.
        //
        ParallelDescriptor::Bcast(&dot, 1, IOProc);
    }

    return dot;
}

inline
Real
qdotxy (const MultiFab& r,
        const MultiFab& z,
        bool            local = false)
{
    return qdotxy(r,0,z,0,local);
}

int
CGSolver::solve_cabicgstab_quad (MultiFab&       sol,
                                 const MultiFab& rhs,
                                 Real            eps_rel,
                                 Real            eps_abs,
                                 LinOp::BC_Mode  bc_mode)
{
    BL_PROFILE("CGSolver::solve_cabicgstab_quad()");

    BL_ASSERT(sol.nComp() == 1);
    BL_ASSERT(sol.boxArray() == Lp.boxArray(lev));
    BL_ASSERT(rhs.boxArray() == Lp.boxArray(lev));

    Real  temp1[4*SSS_MAX+1];
    Real  temp2[4*SSS_MAX+1];
    Real  temp3[4*SSS_MAX+1];
    Real     Tp[4*SSS_MAX+1][4*SSS_MAX+1];
    Real    Tpp[4*SSS_MAX+1][4*SSS_MAX+1];
    Real     aj[4*SSS_MAX+1];
    Real     cj[4*SSS_MAX+1];
    Real     ej[4*SSS_MAX+1];
    Real   Tpaj[4*SSS_MAX+1];
    Real   Tpcj[4*SSS_MAX+1];
    Real  Tppaj[4*SSS_MAX+1];
    Real      G[4*SSS_MAX+1][4*SSS_MAX+1];    // Extracted from first 4*SSS+1 columns of Gg[][].  indexed as [row][col]
    Real      g[4*SSS_MAX+1];                 // Extracted from last [4*SSS+1] column of Gg[][].
    Real     Gg[(4*SSS_MAX+1)*(4*SSS_MAX+2)]; // Buffer to hold the Gram-like matrix produced by matmul().  indexed as [row*(4*SSS+2) + col]
    //
    // If variable_SSS we "telescope" SSS.
    // We start with 1 and increase it up to SSS_MAX on the outer iterations.
    //
    if (variable_SSS) SSS = 1;

    zero(   aj, 4*SSS_MAX+1);
    zero(   cj, 4*SSS_MAX+1);
    zero(   ej, 4*SSS_MAX+1);
    zero( Tpaj, 4*SSS_MAX+1);
    zero( Tpcj, 4*SSS_MAX+1); 
    zero(Tppaj, 4*SSS_MAX+1);
    zero(temp1, 4*SSS_MAX+1);
    zero(temp2, 4*SSS_MAX+1);
    zero(temp3, 4*SSS_MAX+1);

    SetMonomialBasis(Tp,Tpp,SSS);

    const int ncomp = 1, nghost = sol.nGrow();
    //
    // Contains the matrix powers of p[] and r[].
    //
    // First 2*SSS+1 components are powers of p[].
    // Next  2*SSS   components are powers of r[].
    //
    MultiFab PR(sol.boxArray(), 4*SSS_MAX+1, 0);

    MultiFab  p(sol.boxArray(), ncomp, 0);
    MultiFab  r(sol.boxArray(), ncomp, 0);
    MultiFab rt(sol.boxArray(), ncomp, 0);
    
    MultiFab tmp(sol.boxArray(), 4, nghost);

    Lp.residual(r, rhs, sol, lev, bc_mode);

    if ((verbose > 1) && r.contains_nan()) std::cout << "*** r contains NANs\n";

    MultiFab::Copy(rt,r,0,0,1,0);
    MultiFab::Copy( p,r,0,0,1,0);

    const Real           rnorm0        = norm_inf(r);
    Real                 delta         = qdotxy(r,rt);
    const Real           L2_norm_of_rt = sqrt(delta);
    const LinOp::BC_Mode temp_bc_mode  = LinOp::Homogeneous_BC;

    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev);
        std::cout << "CGSolver_CABiCGStab: Initial error (error0) =        " << rnorm0 << '\n';
    }

    if ( rnorm0 == 0 || delta == 0 || rnorm0 < eps_abs )
    {
        if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
	{
            Spacer(std::cout, lev);
            std::cout << "CGSolver_CABiCGStab: niter = 0,"
                      << ", rnorm = "   << rnorm0
                      << ", delta = "   << delta
                      << ", eps_abs = " << eps_abs << '\n';
	}
        return 0;
    }

    int niters = 0, ret = 0;

    Real L2_norm_of_resid = 0, atime = 0, gtime = 0;

    bool BiCGStabFailed = false, BiCGStabConverged = false;

    for (int m = 0; m < maxiter && !BiCGStabFailed && !BiCGStabConverged; )
    {
        const Real time1 = ParallelDescriptor::second();
        //
        // Compute the matrix powers on p[] & r[] (monomial basis).
        // The 2*SSS+1 powers of p[] followed by the 2*SSS powers of r[].
        //
        MultiFab::Copy(PR,p,0,0,1,0);
        MultiFab::Copy(PR,r,0,2*SSS+1,1,0);

        if ((verbose > 1) && PR.contains_nan(0,      1)) std::cout << "*** PR contains NANs @ p\n";
        if ((verbose > 1) && PR.contains_nan(2*SSS+1,1)) std::cout << "*** PR contains NANs @ r\n";
        //
        // We use "tmp" to minimize the number of Lp.apply()s.
        // We do this by doing p & r together in a single call.
        //
        MultiFab::Copy(tmp,p,0,0,1,0);
        MultiFab::Copy(tmp,r,0,1,1,0);

        for (int n = 1; n < 2*SSS; n++)
        {
            Lp.apply(tmp, tmp, lev, temp_bc_mode, false, 0, 2, 2);

            MultiFab::Copy(tmp,tmp,2,0,2,0);

            MultiFab::Copy(PR,tmp,0,        n,1,0);
            MultiFab::Copy(PR,tmp,1,2*SSS+n+1,1,0);

            if ((verbose > 1) && PR.contains_nan(n,        1)) std::cout << "*** PR contains NANs @ p: " << n         << '\n';
            if ((verbose > 1) && PR.contains_nan(2*SSS+n+1,1)) std::cout << "*** PR contains NANs @ r: " << 2*SSS+n+1 << '\n';
        }

        MultiFab::Copy(tmp,PR,2*SSS-1,0,1,0);
        Lp.apply(tmp, tmp, lev, temp_bc_mode, false, 0, 1, 1);
        MultiFab::Copy(PR,tmp,1,2*SSS,1,0);

        if ((verbose > 1) && PR.contains_nan(2*SSS-1,1)) std::cout << "*** PR contains NANs @ 2*SSS-1\n";
        if ((verbose > 1) && PR.contains_nan(2*SSS,  1)) std::cout << "*** PR contains NANs @ 2*SSS\n";

        Real time2 = ParallelDescriptor::second();

        atime += (time2-time1);

        BuildGramMatrix(Gg, PR, rt, SSS);

        const Real time3 = ParallelDescriptor::second();

        gtime += (time3-time2);
        //
        // Form G[][] and g[] from Gg.
        //
        for (int i = 0, k = 0; i < 4*SSS+1; i++)
        {
            for (int j = 0; j < 4*SSS+1; j++)
                //
                // First 4*SSS+1 elements in each row go to G[][].
                //
                G[i][j] = Gg[k++];
            //
            // Last element in row goes to g[].
            //
            g[i] = Gg[k++];
        }

        zero(aj, 4*SSS+1); aj[0]       = 1;
        zero(cj, 4*SSS+1); cj[2*SSS+1] = 1;
        zero(ej, 4*SSS+1);

        for (int nit = 0; nit < SSS; nit++)
        {
            qgemv( Tpaj,  Tp, aj, 4*SSS+1, 4*SSS+1);
            qgemv( Tpcj,  Tp, cj, 4*SSS+1, 4*SSS+1);
            qgemv(Tppaj, Tpp, aj, 4*SSS+1, 4*SSS+1);

            const Real g_dot_Tpaj = qdot(g, Tpaj, 4*SSS+1);

            if ( g_dot_Tpaj == 0 )
            {
                if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: g_dot_Tpaj == 0, nit = " << nit << '\n';
                BiCGStabFailed = true; ret = 1; break;
            }

            const Real alpha = delta / g_dot_Tpaj;

            if ( isinf(alpha) )
            {
                if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: alpha == inf, nit = " << nit << '\n';
                BiCGStabFailed = true; ret = 2; break;
            }

            axpy(temp1, Tpcj, -alpha, Tppaj, 4*SSS+1);

            qgemv(temp2, G, temp1, 4*SSS+1, 4*SSS+1);

            axpy(temp3,   cj, -alpha,  Tpaj, 4*SSS+1);

            const Real omega_numerator   = qdot(temp3, temp2, 4*SSS+1);
            const Real omega_denominator = qdot(temp1, temp2, 4*SSS+1);
            //
            // NOTE: omega_numerator/omega_denominator can be 0/x or 0/0, but should never be x/0.
            //
            // If omega_numerator==0, and ||s||==0, then convergence, x=x+alpha*aj.
            // If omega_numerator==0, and ||s||!=0, then stabilization breakdown.
            //
            // Partial update of ej must happen before the check on omega to ensure forward progress !!!
            //
            axpy(ej, ej, alpha, aj, 4*SSS+1);
            //
            // ej has been updated so consider that we've done an iteration since
            // even if we break out of the loop we'll be able to update both sol.
            //
            niters++;
            //
            // Calculate the norm of Saad's vector 's' to check intra s-step convergence.
            //
            axpy(temp1, cj,-alpha,  Tpaj, 4*SSS+1);

            qgemv(temp2, G, temp1, 4*SSS+1, 4*SSS+1);

            const Real L2_norm_of_s = qdot(temp1,temp2,4*SSS+1);

            L2_norm_of_resid = (L2_norm_of_s < 0 ? 0 : sqrt(L2_norm_of_s));

            if ( L2_norm_of_resid < eps_rel*L2_norm_of_rt )
            {
                if ( verbose > 1 && L2_norm_of_resid == 0 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: L2 norm of s: " << L2_norm_of_s << '\n';
                BiCGStabConverged = true; break;
            }

            if ( omega_denominator == 0 )
            {
                if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: omega_denominator == 0, nit = " << nit << '\n';
                BiCGStabFailed = true; ret = 3; break;
            }

            const Real omega = omega_numerator / omega_denominator;

            if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
            {
                if ( omega == 0   ) std::cout << "CGSolver_CABiCGStab: omega == 0, nit = " << nit << '\n';
                if ( isinf(omega) ) std::cout << "CGSolver_CABiCGStab: omega == inf, nit = " << nit << '\n';
            }

            if ( omega == 0   ) { BiCGStabFailed = true; ret = 4; break; }
            if ( isinf(omega) ) { BiCGStabFailed = true; ret = 4; break; }
            //
            // Complete the update of ej & cj now that omega is known to be ok.
            //
            axpy(ej, ej,       omega,    cj, 4*SSS+1);
            axpy(ej, ej,-omega*alpha,  Tpaj, 4*SSS+1);
            axpy(cj, cj,      -omega,  Tpcj, 4*SSS+1);
            axpy(cj, cj,      -alpha,  Tpaj, 4*SSS+1);
            axpy(cj, cj, omega*alpha, Tppaj, 4*SSS+1);
            //
            // Do an early check of the residual to determine convergence.
            //
            qgemv(temp1, G, cj, 4*SSS+1, 4*SSS+1);
            //
            // sqrt( (cj,Gcj) ) == L2 norm of the intermediate residual in exact arithmetic.
            // However, finite precision can lead to the norm^2 being < 0 (Jim Demmel).
            // If cj_dot_Gcj < 0 we flush to zero and consider ourselves converged.
            //
            const Real L2_norm_of_r = qdot(cj, temp1, 4*SSS+1);

            L2_norm_of_resid = (L2_norm_of_r > 0 ? sqrt(L2_norm_of_r) : 0);

            if ( L2_norm_of_resid < eps_rel*L2_norm_of_rt )
            {
                if ( verbose > 1 && L2_norm_of_resid == 0 && ParallelDescriptor::IOProcessor() )
                    std::cout << "CGSolver_CABiCGStab: L2_norm_of_r: " << L2_norm_of_r << '\n';
                BiCGStabConverged = true; break;
            }

            const Real delta_next = qdot(g, cj, 4*SSS+1);

            if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
            {
                if ( delta_next == 0   ) std::cout << "CGSolver_CABiCGStab: delta == 0, nit = " << nit << '\n';
                if ( isinf(delta_next) ) std::cout << "CGSolver_CABiCGStab: delta == inf, nit = " << nit << '\n';
            }

            if ( isinf(delta_next) ) { BiCGStabFailed = true; ret = 5; break; } // delta = inf?
            if ( delta_next  == 0  ) { BiCGStabFailed = true; ret = 5; break; } // Lanczos breakdown...

            const Real beta = (delta_next/delta)*(alpha/omega);

            if ( verbose > 1 && ParallelDescriptor::IOProcessor() )
            {
                if ( beta == 0   ) std::cout << "CGSolver_CABiCGStab: beta == 0, nit = " << nit << '\n';
                if ( isinf(beta) ) std::cout << "CGSolver_CABiCGStab: beta == inf, nit = " << nit << '\n';
            }

            if ( isinf(beta) ) { BiCGStabFailed = true; ret = 6; break; } // beta = inf?
            if ( beta == 0   ) { BiCGStabFailed = true; ret = 6; break; } // beta = 0?  can't make further progress(?)

            axpy(aj, cj,        beta,   aj, 4*SSS+1);
            axpy(aj, aj, -omega*beta, Tpaj, 4*SSS+1);

            delta = delta_next;
        }
        //
        // Update iterates.
        //
        for (int i = 0; i < 4*SSS+1; i++)
            sxay(sol,sol,ej[i],PR,i);

        MultiFab::Copy(p,PR,0,0,1,0);
        p.mult(aj[0],0,1);
        for (int i = 1; i < 4*SSS+1; i++)
            sxay(p,p,aj[i],PR,i);

        MultiFab::Copy(r,PR,0,0,1,0);
        r.mult(cj[0],0,1);
        for (int i = 1; i < 4*SSS+1; i++)
            sxay(r,r,cj[i],PR,i);

        if ( !BiCGStabFailed && !BiCGStabConverged )
        {
            m += SSS;

            if ( variable_SSS && SSS < SSS_MAX ) { SSS++; SetMonomialBasis(Tp,Tpp,SSS); }
        }
    }

    if ( verbose > 0 )
    {
        if ( ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev);
            std::cout << "CGSolver_CABiCGStab: Final: Iteration "
                      << std::setw(4) << niters
                      << " rel. err. "
                      << L2_norm_of_resid << '\n';
        }

        if ( verbose > 1 )
        {
            Real tmp[2] = { atime, gtime };

            ParallelDescriptor::ReduceRealMax(tmp,2,ParallelDescriptor::IOProcessorNumber());

            if ( ParallelDescriptor::IOProcessor() )
            {
                Spacer(std::cout, lev);
                std::cout << "CGSolver_CABiCGStab apply time: " << tmp[0] << ", gram time: " << tmp[1] << '\n';
            }
        }
    }

    if ( niters == maxiter && !BiCGStabFailed && !BiCGStabConverged)
    {
        if ( L2_norm_of_resid > L2_norm_of_rt )
        {
            if ( ParallelDescriptor::IOProcessor() )
                BoxLib::Warning("CGSolver_CABiCGStab: failed to converge!");
            //
            // Return code 8 tells the MultiGrid driver to zero out the solution!
            //
            ret = 8;
        }
        else
        {
            //
            // Return codes 1-7 tells the MultiGrid driver to smooth the solution!
            //
            ret = 7;
        }
    }

    return ret;
}

#endif /*XBLAS*/

static
void
BuildGramMatrix (Real*           Gg,
                 const MultiFab& PR,
                 const MultiFab& rt,
                 int             sss)
{
    BL_ASSERT(rt.nComp() == 1);
    BL_ASSERT(PR.nComp() >= 4*sss+1);

    const int Nrows = 4*sss+1, Ncols = Nrows + 1;

    Real tmp[ ((4*SSS_MAX+2)*(4*SSS_MAX+3)) / 2 - 1 ];
    //
    // Gg is dimensioned (Ncols*Nrows).
    //
    // First fill the upper triangle..
    //
#ifdef _OPENMP
#pragma omp parallel for schedule(static,1)
#endif
    for (int mm = 0; mm < Nrows; mm++)
    {
        for (int nn = mm; nn < Nrows; nn++)
        {
#ifdef XBLAS
            Gg[mm*Ncols + nn] = qdotxy(PR, mm, PR, nn, true);
#else
            Gg[mm*Ncols + nn] = dotxy(PR, mm, PR, nn, true);
#endif
        }

#ifdef XBLAS
        Gg[mm*Ncols + Nrows] = qdotxy(PR, mm, rt, 0, true);
#else
        Gg[mm*Ncols + Nrows] = dotxy(PR, mm, rt, 0, true);
#endif
    }
    //
    // Next put the upper triangle into "tmp" for reduction.
    //
    int cnt = 0;

    for (int mm = 0; mm < Nrows; mm++)
    {
        for (int nn = mm; nn < Nrows; nn++)
        {
            tmp[cnt++] = Gg[mm*Ncols + nn];
        }

        tmp[cnt++] = Gg[mm*Ncols + Nrows];
    }

    ParallelDescriptor::ReduceRealSum(tmp, cnt);
    //
    // Now refill upper triangle with "tmp".
    //
    cnt = 0;

    for (int mm = 0; mm < Nrows; mm++)
    {
        for (int nn = mm; nn < Nrows; nn++)
        {
            Gg[mm*Ncols + nn] = tmp[cnt++];
        }

        Gg[mm*Ncols + Nrows] = tmp[cnt++];
    }
    //
    // Then fill in strict lower triangle using symmetry.
    //
    for (int mm = 0; mm < Nrows; mm++)
    {
        for (int nn = 0; nn < mm; nn++)
        {
            Gg[mm*Ncols + nn] = Gg[nn*Ncols + mm];
        }
    }
}

int
CGSolver::solve_bicgstab (MultiFab&       sol,
                          const MultiFab& rhs,
                          Real            eps_rel,
                          Real            eps_abs,
                          LinOp::BC_Mode  bc_mode)
{
    BL_PROFILE("CGSolver::solve_bicgstab()");

    const int nghost = sol.nGrow(), ncomp = 1;

    BL_ASSERT(sol.nComp() == ncomp);
    BL_ASSERT(sol.boxArray() == Lp.boxArray(lev));
    BL_ASSERT(rhs.boxArray() == Lp.boxArray(lev));

    MultiFab ph(sol.boxArray(), ncomp, nghost);
    MultiFab sh(sol.boxArray(), ncomp, nghost);

    MultiFab sorig(sol.boxArray(), ncomp, 0);
    MultiFab p    (sol.boxArray(), ncomp, 0);
    MultiFab r    (sol.boxArray(), ncomp, 0);
    MultiFab s    (sol.boxArray(), ncomp, 0);
    MultiFab rh   (sol.boxArray(), ncomp, 0);
    MultiFab v    (sol.boxArray(), ncomp, 0);
    MultiFab t    (sol.boxArray(), ncomp, 0);

    Lp.residual(r, rhs, sol, lev, bc_mode);

    MultiFab::Copy(sorig,sol,0,0,1,0);
    MultiFab::Copy(rh,   r,  0,0,1,0);

    sol.setVal(0);

    const LinOp::BC_Mode temp_bc_mode = LinOp::Homogeneous_BC;

#ifdef CG_USE_OLD_CONVERGENCE_CRITERIA
    Real rnorm = norm_inf(r);
#else
    //
    // Calculate the local values of these norms & reduce their values together.
    //
    Real vals[2] = { norm_inf(r, true), Lp.norm(0, lev, true) };

    ParallelDescriptor::ReduceRealMax(vals,2);

    Real       rnorm    = vals[0];
    const Real Lp_norm  = vals[1];
    Real       sol_norm = 0;
#endif
    const Real rnorm0   = rnorm;

    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev);
        std::cout << "CGSolver_BiCGStab: Initial error (error0) =        " << rnorm0 << '\n';
    }
    int ret = 0, nit = 1;
    Real rho_1 = 0, alpha = 0, omega = 0;

    if ( rnorm0 == 0 || rnorm0 < eps_abs )
    {
        if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
	{
            Spacer(std::cout, lev);
            std::cout << "CGSolver_BiCGStab: niter = 0,"
                      << ", rnorm = " << rnorm 
                      << ", eps_abs = " << eps_abs << std::endl;
	}
        return ret;
    }

    for (; nit <= maxiter; ++nit)
    {
        const Real rho = dotxy(rh,r);
        if ( rho == 0 ) 
	{
            ret = 1; break;
	}
        if ( nit == 1 )
        {
            MultiFab::Copy(p,r,0,0,1,0);
        }
        else
        {
            const Real beta = (rho/rho_1)*(alpha/omega);
            sxay(p, p, -omega, v);
            sxay(p, r,   beta, p);
        }
        if ( use_mg_precond )
        {
            ph.setVal(0);
            mg_precond->solve(ph, p, eps_rel, eps_abs, temp_bc_mode);
        }
        else if ( use_jacobi_precond )
        {
            ph.setVal(0);
            Lp.jacobi_smooth(ph, p, lev, temp_bc_mode);
        }
        else 
        {
            MultiFab::Copy(ph,p,0,0,1,0);
        }
        Lp.apply(v, ph, lev, temp_bc_mode);

        if ( Real rhTv = dotxy(rh,v) )
	{
            alpha = rho/rhTv;
	}
        else
	{
            ret = 2; break;
	}
        sxay(sol, sol,  alpha, ph);
        sxay(s,     r, -alpha,  v);

        rnorm = norm_inf(s);

        if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev);
            std::cout << "CGSolver_BiCGStab: Half Iter "
                      << std::setw(11) << nit
                      << " rel. err. "
                      << rnorm/(rnorm0) << '\n';
        }

#ifdef CG_USE_OLD_CONVERGENCE_CRITERIA
        if ( rnorm < eps_rel*rnorm0 || rnorm < eps_abs ) break;
#else
        sol_norm = norm_inf(sol);
        if ( rnorm < eps_rel*(Lp_norm*sol_norm + rnorm0 ) || rnorm < eps_abs ) break;
#endif
        if ( use_mg_precond )
        {
            sh.setVal(0);
            mg_precond->solve(sh, s, eps_rel, eps_abs, temp_bc_mode);
        }
        else if ( use_jacobi_precond )
        {
            sh.setVal(0);
            Lp.jacobi_smooth(sh, s, lev, temp_bc_mode);
        }
        else
        {
            MultiFab::Copy(sh,s,0,0,1,0);
        }
        Lp.apply(t, sh, lev, temp_bc_mode);
        //
        // This is a little funky.  I want to elide one of the reductions
        // in the following two dotxy()s.  We do that by calculating the "local"
        // values and then reducing the two local values at the same time.
        //
        Real vals[2] = { dotxy(t,t,true), dotxy(t,s,true) };

        ParallelDescriptor::ReduceRealSum(vals,2);

        if ( vals[0] )
	{
            omega = vals[1]/vals[0];
	}
        else
	{
            ret = 3; break;
	}
        sxay(sol, sol,  omega, sh);
        sxay(r,     s, -omega,  t);

        rnorm = norm_inf(r);

        if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev);
            std::cout << "CGSolver_BiCGStab: Iteration "
                      << std::setw(11) << nit
                      << " rel. err. "
                      << rnorm/(rnorm0) << '\n';
        }

#ifdef CG_USE_OLD_CONVERGENCE_CRITERIA
        if ( rnorm < eps_rel*rnorm0 || rnorm < eps_abs ) break;
#else
        sol_norm = norm_inf(sol);
        if ( rnorm < eps_rel*(Lp_norm*sol_norm + rnorm0 ) || rnorm < eps_abs ) break;
#endif
        if ( omega == 0 )
	{
            ret = 4; break;
	}
        rho_1 = rho;
    }

    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev);
        std::cout << "CGSolver_BiCGStab: Final: Iteration "
                  << std::setw(4) << nit
                  << " rel. err. "
                  << rnorm/(rnorm0) << '\n';
    }

#ifdef CG_USE_OLD_CONVERGENCE_CRITERIA
    if ( ret == 0 && rnorm > eps_rel*rnorm0 && rnorm > eps_abs)
#else
    if ( ret == 0 && rnorm > eps_rel*(Lp_norm*sol_norm + rnorm0 ) && rnorm > eps_abs )
#endif
    {
        if ( ParallelDescriptor::IOProcessor() )
            BoxLib::Warning("CGSolver_BiCGStab:: failed to converge!");
        ret = 8;
    }

    if ( ( ret == 0 || ret == 8 ) && (rnorm < rnorm0) )
    {
        sol.plus(sorig, 0, 1, 0);
    } 
    else 
    {
        sol.setVal(0);
        sol.plus(sorig, 0, 1, 0);
    }

    return ret;
}

int
CGSolver::solve_cg (MultiFab&       sol,
		    const MultiFab& rhs,
		    Real            eps_rel,
		    Real            eps_abs,
		    LinOp::BC_Mode  bc_mode)
{
    BL_PROFILE("CGSolver::solve_cg()");

    const int nghost = sol.nGrow(), ncomp = 1;

    BL_ASSERT(sol.nComp() == ncomp);
    BL_ASSERT(sol.boxArray() == Lp.boxArray(lev));
    BL_ASSERT(rhs.boxArray() == Lp.boxArray(lev));

    MultiFab sorig(sol.boxArray(), ncomp, nghost);
    MultiFab r(sol.boxArray(), ncomp, nghost);
    MultiFab z(sol.boxArray(), ncomp, nghost);
    MultiFab q(sol.boxArray(), ncomp, nghost);
    MultiFab p(sol.boxArray(), ncomp, nghost);

    MultiFab r1(sol.boxArray(), ncomp, nghost);
    MultiFab z1(sol.boxArray(), ncomp, nghost);
    MultiFab r2(sol.boxArray(), ncomp, nghost);
    MultiFab z2(sol.boxArray(), ncomp, nghost);

    MultiFab::Copy(sorig,sol,0,0,1,0);

    Lp.residual(r, rhs, sorig, lev, bc_mode);

    sol.setVal(0);

    const LinOp::BC_Mode temp_bc_mode=LinOp::Homogeneous_BC;

    Real       rnorm    = norm_inf(r);
    const Real rnorm0   = rnorm;
    Real       minrnorm = rnorm;

    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev);
        std::cout << "              CG: Initial error :        " << rnorm0 << '\n';
    }

    const Real Lp_norm = Lp.norm(0, lev);
    Real sol_norm      = 0;
    Real rho_1         = 0;
    int  ret           = 0;
    int  nit           = 1;

    if ( rnorm == 0 || rnorm < eps_abs )
    {
        if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
	{
            Spacer(std::cout, lev);
            std::cout << "       CG: niter = 0,"
                      << ", rnorm = " << rnorm 
                      << ", eps_rel*(Lp_norm*sol_norm + rnorm0 )" <<  eps_rel*(Lp_norm*sol_norm + rnorm0 ) 
                      << ", eps_abs = " << eps_abs << std::endl;
	}
        return 0;
    }

    for (; nit <= maxiter; ++nit)
    {
        if (use_jbb_precond && ParallelDescriptor::NProcs() > 1)
        {
            z.setVal(0);

            jbb_precond(z,r,lev,Lp);
        }
        else
        {
            MultiFab::Copy(z,r,0,0,1,0);
        }

        Real rho = dotxy(z,r);

        if (nit == 1)
        {
            MultiFab::Copy(p,z,0,0,1,0);
        }
        else
        {
            Real beta = rho/rho_1;
            sxay(p, z, beta, p);
        }
        Lp.apply(q, p, lev, temp_bc_mode);

        Real alpha;
        if ( Real pw = dotxy(p,q) )
	{
            alpha = rho/pw;
	}
        else
	{
            ret = 1; break;
	}
        
        if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev);
            std::cout << "CGSolver_cg:"
                      << " nit " << nit
                      << " rho " << rho
                      << " alpha " << alpha << '\n';
        }
        sxay(sol, sol, alpha, p);
        sxay(  r,   r,-alpha, q);
        rnorm = norm_inf(r);
        sol_norm = norm_inf(sol);

        if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev);
            std::cout << "       CG:       Iteration"
                      << std::setw(4) << nit
                      << " rel. err. "
                      << rnorm/(rnorm0) << '\n';
        }

#ifdef CG_USE_OLD_CONVERGENCE_CRITERIA
        if ( rnorm < eps_rel*rnorm0 || rnorm < eps_abs ) break;
#else
        if ( rnorm < eps_rel*(Lp_norm*sol_norm + rnorm0) || rnorm < eps_abs ) break;
#endif
        if ( rnorm > def_unstable_criterion*minrnorm )
	{
            ret = 2; break;
	}
        else if ( rnorm < minrnorm )
	{
            minrnorm = rnorm;
	}

        rho_1 = rho;
    }
    
    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev);
        std::cout << "       CG: Final Iteration"
                  << std::setw(4) << nit
                  << " rel. err. "
                  << rnorm/(rnorm0) << '\n';
    }

#ifdef CG_USE_OLD_CONVERGENCE_CRITERIA
    if ( ret == 0 &&  rnorm > eps_rel*rnorm0 && rnorm > eps_abs )
#else
    if ( ret == 0 && rnorm > eps_rel*(Lp_norm*sol_norm + rnorm0) && rnorm > eps_abs )
#endif
    {
        if ( ParallelDescriptor::IOProcessor() )
            BoxLib::Warning("CGSolver_cg: failed to converge!");
        ret = 8;
    }

    if ( ( ret == 0 || ret == 8 ) && (rnorm < rnorm0) )
    {
        sol.plus(sorig, 0, 1, 0);
    } 
    else 
    {
        sol.setVal(0);
        sol.plus(sorig, 0, 1, 0);
    }

    return ret;
}

int
CGSolver::jbb_precond (MultiFab&       sol,
		       const MultiFab& rhs,
                       int             lev,
		       LinOp&          Lp)
{
    //
    // This is a local routine.  No parallel is allowed to happen here.
    //
    int                  lev_loc = lev;
    const Real           eps_rel = 1.e-2;
    const Real           eps_abs = 1.e-16;
    const int            nghost  = sol.nGrow();
    const int            ncomp   = sol.nComp();
    const bool           local   = true;
    const LinOp::BC_Mode bc_mode = LinOp::Homogeneous_BC;

    BL_ASSERT(ncomp == 1 );
    BL_ASSERT(sol.boxArray() == Lp.boxArray(lev_loc));
    BL_ASSERT(rhs.boxArray() == Lp.boxArray(lev_loc));

    MultiFab sorig(sol.boxArray(), ncomp, nghost);

    MultiFab r(sol.boxArray(), ncomp, nghost);
    MultiFab z(sol.boxArray(), ncomp, nghost);
    MultiFab q(sol.boxArray(), ncomp, nghost);
    MultiFab p(sol.boxArray(), ncomp, nghost);

    sorig.copy(sol);

    Lp.residual(r, rhs, sorig, lev_loc, LinOp::Homogeneous_BC, local);

    sol.setVal(0);

    Real       rnorm    = norm_inf(r,local);
    const Real rnorm0   = rnorm;
    Real       minrnorm = rnorm;

    if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev_loc);
        std::cout << "     jbb_precond: Initial error :        " << rnorm0 << '\n';
    }

    const Real Lp_norm = Lp.norm(0, lev_loc, local);
    Real sol_norm = 0;
    int  ret      = 0;			// will return this value if all goes well
    Real rho_1    = 0;
    int  nit      = 1;

    if ( rnorm0 == 0 || rnorm0 < eps_abs )
    {
        if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
	{
            Spacer(std::cout, lev_loc);
            std::cout << "jbb_precond: niter = 0,"
                      << ", rnorm = " << rnorm 
                      << ", eps_abs = " << eps_abs << std::endl;
	}
        return 0;
    }

    for (; nit <= maxiter; ++nit)
    {
        z.copy(r);

        Real rho = dotxy(z,r,local);
        if (nit == 1)
        {
            p.copy(z);
        }
        else
        {
            Real beta = rho/rho_1;
            sxay(p, z, beta, p);
        }

        Lp.apply(q, p, lev_loc, bc_mode, local);

        Real alpha;
        if ( Real pw = dotxy(p,q,local) )
	{
            alpha = rho/pw;
	}
        else
	{
            ret = 1; break;
	}
        
        if ( verbose > 3 && ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev_loc);
            std::cout << "jbb_precond:" << " nit " << nit
                      << " rho " << rho << " alpha " << alpha << '\n';
        }
        sxay(sol, sol, alpha, p);
        sxay(  r,   r,-alpha, q);
        rnorm    = norm_inf(r,   local);
        sol_norm = norm_inf(sol, local);

        if ( verbose > 2 && ParallelDescriptor::IOProcessor() )
        {
            Spacer(std::cout, lev_loc);
            std::cout << "jbb_precond:       Iteration"
                      << std::setw(4) << nit
                      << " rel. err. "
                      << rnorm/(rnorm0) << '\n';
        }

        if ( rnorm < eps_rel*(Lp_norm*sol_norm + rnorm0) || rnorm < eps_abs )
	{
            break;
	}
      
        if ( rnorm > def_unstable_criterion*minrnorm )
	{
            ret = 2; break;
	}
        else if ( rnorm < minrnorm )
	{
            minrnorm = rnorm;
	}

        rho_1 = rho;
    }
    
    if ( verbose > 0 && ParallelDescriptor::IOProcessor() )
    {
        Spacer(std::cout, lev_loc);
        std::cout << "jbb_precond: Final Iteration"
                  << std::setw(4) << nit
                  << " rel. err. "
                  << rnorm/(rnorm0) << '\n';
    }
    if ( ret == 0 && rnorm > eps_rel*(Lp_norm*sol_norm + rnorm0) && rnorm > eps_abs )
    {
        if ( ParallelDescriptor::IOProcessor() )
	{
            BoxLib::Warning("jbb_precond:: failed to converge!");
	}
        ret = 8;
    }

    if ( ( ret == 0 || ret == 8 ) && (rnorm < rnorm0) )
    {
        sol.plus(sorig, 0, 1, 0);
    } 
    else 
    {
        sol.setVal(0);
        sol.plus(sorig, 0, 1, 0);
    }

    return ret;
}
