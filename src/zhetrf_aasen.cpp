/*
    -- MAGMA (version 1.1) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       @date

       @author Stan Tomov
       @precisions normal z -> s d c
*/
#include "common_magma.h"
#include "trace.h"
extern "C" void
magmablas_zlaswp_sym( magma_int_t n, magmaDoubleComplex *dA, magma_int_t lda,
                      magma_int_t k1, magma_int_t k2,
                      const magma_int_t *ipiv, magma_int_t inci );
extern "C" void
magmablas_zlacpy_sym_in(
    magma_uplo_t uplo, magma_int_t m, magma_int_t n,
    magma_int_t *rows, magma_int_t *perm,
    magmaDoubleComplex_const_ptr dA, magma_int_t ldda,
    magmaDoubleComplex_ptr       dB, magma_int_t lddb );
extern "C" void
magmablas_zlacpy_sym_out(
    magma_uplo_t uplo, magma_int_t m, magma_int_t n,
    magma_int_t *rows, magma_int_t *perm,
    magmaDoubleComplex_const_ptr dA, magma_int_t ldda,
    magmaDoubleComplex_ptr       dB, magma_int_t lddb );
#define PRECISION_z

// === Define what BLAS to use ============================================
#define PRECISION_z
#if (defined(PRECISION_s) || defined(PRECISION_d))
  #define cublasZgemm magmablas_zgemm
  #define cublasZtrsm magmablas_ztrsm
#endif

#if (GPUSHMEM >= 200)
#if (defined(PRECISION_s))
     #undef  cublasSgemm
     #define cublasSgemm magmablas_sgemm_fermi80
  #endif
#endif
// === End defining what BLAS to use ======================================

/**
    Purpose
    -------
    ZPOTRF computes the Cholesky factorization of a complex Hermitian
    positive definite matrix A. This version does not require work
    space on the GPU passed as input. GPU memory is allocated in the
    routine.

    The factorization has the form
        A = U**H * U,  if uplo = MagmaUpper, or
        A = L  * L**H, if uplo = MagmaLower,
    where U is an upper triangular matrix and L is lower triangular.

    This is the block version of the algorithm, calling Level 3 BLAS.
    
    If the current stream is NULL, this version replaces it with a new
    stream to overlap computation with communication.

    Arguments
    ---------
    @param[in]
    uplo    magma_uplo_t
      -     = MagmaUpper:  Upper triangle of A is stored;
      -     = MagmaLower:  Lower triangle of A is stored.

    @param[in]
    n       INTEGER
            The order of the matrix A.  N >= 0.

    @param[in,out]
    A       COMPLEX_16 array, dimension (LDA,N)
            On entry, the Hermitian matrix A.  If uplo = MagmaUpper, the leading
            N-by-N upper triangular part of A contains the upper
            triangular part of the matrix A, and the strictly lower
            triangular part of A is not referenced.  If uplo = MagmaLower, the
            leading N-by-N lower triangular part of A contains the lower
            triangular part of the matrix A, and the strictly upper
            triangular part of A is not referenced.
    \n
            On exit, if INFO = 0, the factor U or L from the Cholesky
            factorization A = U**H * U or A = L * L**H.
    \n
            Higher performance is achieved if A is in pinned memory, e.g.
            allocated using magma_malloc_pinned.

    @param[in]
    lda     INTEGER
            The leading dimension of the array A.  LDA >= max(1,N).

    @param[out]
    info    INTEGER
      -     = 0:  successful exit
      -     < 0:  if INFO = -i, the i-th argument had an illegal value
                  or another error occured, such as memory allocation failed.
      -     > 0:  if INFO = i, the leading minor of order i is not
                  positive definite, and the factorization could not be
                  completed.

    @ingroup magma_zposv_comp
    ********************************************************************/
extern "C" magma_int_t
magma_zhetrf_aasen(magma_uplo_t uplo, magma_int_t cpu_panel, magma_int_t n,
                   magmaDoubleComplex *A, magma_int_t lda, 
                   magma_int_t *ipiv, magma_int_t *info)
{
#define A(i, j)  (A    + (j)*nb*lda  + (i)*nb)
#define dA(i, j) (work + (j)*nb*ldda + (i)*nb)
#define dT(i, j) (work + (j)*nb*ldda + (i)*nb)
#define dL(i, j) ((i == j) ? (dL + (i)*nb) : (work + (j-1)*nb*ldda + (i)*nb))
#define dH(i, j) (dH   + (i)*nb)
#define dW(i)    (dW   + (i)*nb*nb)
#define dX(i)    (dX   + (i)*nb*nb)
#define dY(i)    (dY   + (i)*nb*nb)
//#define dW(i)    (dW   + (i)*nb)

#define da(i, j) (work + (j)*ldda + (i))
#define dw(i)    (dW   + (i))
#define dl(i,j)  (work + (i) + (j)*ldda)

    /* Local variables */
    magma_int_t        ldda, iinfo;
    double d_one  = 1.0;
    magmaDoubleComplex    c_one  = MAGMA_Z_ONE;
    magmaDoubleComplex    c_zero = MAGMA_Z_ZERO;
    magmaDoubleComplex    c_mone = MAGMA_Z_NEG_ONE;
    magmaDoubleComplex    c_half = MAGMA_Z_MAKE(0.5, 0.0);
    magmaDoubleComplex   *work, *dH, *dW, *dX, *dY, *dL;
    magma_int_t nb = magma_get_zhetrf_aasen_nb(n);
    int upper = (uplo == MagmaUpper);

    *info = 0;
    if (! upper && uplo != MagmaLower) {
        *info = -1;
    } else if (n < 0) {
        *info = -2;
    } else if (lda < max(1,n)) {
        *info = -4;
    }
    if (*info != 0) {
        magma_xerbla( __func__, -(*info) );
        return *info;
    }

    /* Quick return */
    if ( n == 0 )
        return *info;

    /* Define user stream if current stream is NULL */
    magma_int_t num_streams = 3;
    magma_queue_t stream[5];
    magma_event_t event[5];
    magma_queue_t orig_stream;
    magmablasGetKernelStream( &orig_stream );
    for (int i=0; i<num_streams; i++) {
        magma_queue_create( &stream[i] );
        magma_event_create( &event[i]  );
    }
    magmablasSetKernelStream(stream[0]);

    int lddw = nb*(1+(n+nb-1)/nb);
    ldda = ((n+31)/32)*32;
    if (MAGMA_SUCCESS != magma_zmalloc( &work, (nb*((n+nb-1)/nb)) *ldda ) ||
        MAGMA_SUCCESS != magma_zmalloc( &dH,   (2*nb)*ldda ) ||
        MAGMA_SUCCESS != magma_zmalloc( &dL,   nb*ldda ) ||
        MAGMA_SUCCESS != magma_zmalloc( &dX,   nb*lddw ) ||
        MAGMA_SUCCESS != magma_zmalloc( &dY,   nb*lddw ) ||
        MAGMA_SUCCESS != magma_zmalloc( &dW,   nb*lddw )) {
        /* alloc failed for workspace */
        *info = MAGMA_ERR_DEVICE_ALLOC;
        return *info;
    }
    lddw = nb;

    /* permutation info */
    int *perm, *rows;
    int *drows, *dperm;
    if (MAGMA_SUCCESS != magma_imalloc_cpu(&perm, n) ||
        MAGMA_SUCCESS != magma_imalloc_pinned(&rows, 2*(2*nb)) ||
        MAGMA_SUCCESS != magma_imalloc( &drows, 2*(2*nb) ) ||
        MAGMA_SUCCESS != magma_imalloc( &dperm, n ) ) {
        /* alloc failed for perm info */
        *info = MAGMA_ERR_DEVICE_ALLOC;
        return *info;
    }

    magma_int_t *dinfo_magma, *dipiv_magma;
    magmaDoubleComplex **dA_array = NULL;
    magma_int_t **dipiv_array = NULL;
    if(MAGMA_SUCCESS != magma_imalloc( &dipiv_magma, nb) ||
       MAGMA_SUCCESS != magma_imalloc( &dinfo_magma, 1) ||
       MAGMA_SUCCESS != magma_malloc( (void**)&dA_array, sizeof(*dA_array) ) ||
       MAGMA_SUCCESS != magma_malloc( (void**)&dipiv_array, sizeof(*dipiv_array))) {
        /* alloc failed for perm info */
        *info = MAGMA_ERR_DEVICE_ALLOC;
        return *info;
    }

    for (int ii=0; ii<n; ii++) perm[ii] = ii;
    magma_isetvector_async(n, perm, 1, dperm, 1, stream[0]);

    /* copy A to GPU */
    magma_zsetmatrix_async( n, n, A(0,0), lda, dA(0,0), ldda, stream[0] );
    for (int j=0; j<min(n,nb); j++) ipiv[j] = j+1;

    trace_init( 1, 1, num_streams, (CUstream_st**)stream );
    //if (nb <= 1 || nb >= n) {
    //    lapackf77_zpotrf(uplo_, &n, A, &lda, info);
    //} else 
    {
        /* Use hybrid blocked code. */
        if (upper) {
        }
        else {
            //=========================================================
            // Compute the Cholesky factorization A = L*L'.
            for (int j=0; j < (n+nb-1)/nb; j ++) {
                int jb = min(nb, (n-j*nb));

                // Compute off-diagonal blocks of H(:,j), 
                // i.e., H(i,j) = T(i,i-1)*L(j,i-1)' + T(i,i)*L(j,i)' + T(i,i+1)*L(j,i+1)'
                // H(0,j) and W(0) is not needed since they are multiplied with L(2:N,1)
                // * make sure stream[1] does not start before stream[0] finish everything
                magma_event_record( event[1], stream[0] );
                magma_queue_wait_event( stream[1], event[1] );
                //
                trace_gpu_start( 0, 0, "gemm", "compH" );
                trace_gpu_start( 0, 1, "gemm", "compH" );
                for (int i=1; i<j; i++)
                {
                    //printf( " > compute H(%d,%d)\n",i,j);
                    // > H(i,j) = T(i,i) * L(j,i)', Y
                    magmablasSetKernelStream(stream[0]);
                    magma_zgemm(MagmaNoTrans, MagmaConjTrans,
                                nb, jb, nb,
                                c_one,  dT(i,i), ldda,
                                        dL(j,i), ldda,
                                c_zero, dX(i),   nb);
                                //c_zero, dW(i+1), lddw);
                    // > W(i) = T(i,i+1) * L(j,i+1)', Z
                    // W(i) = L(j,i+1)'
                    magmablasSetKernelStream(stream[1]);
                    magma_zgemm(MagmaConjTrans, MagmaConjTrans,
                                nb, jb, nb,
                                c_one,  dT(i+1,i), ldda,
                                        dL(j,i+1), ldda,
                                c_zero, dH(i,j),   ldda);
                }
                // * insert event to keep track 
                magma_event_record( event[0], stream[0] );
                magma_event_record( event[1], stream[1] );
                // * make sure they are done
                magma_queue_wait_event( stream[0], event[1] );
                magma_queue_wait_event( stream[1], event[0] );
                for (int i=1; i<j; i++)
                {
                    // H(i,j) = W(i)+.5*H(i,j)
                    magmablasSetKernelStream(stream[(i-1)%2]);
                    magmablas_zgeadd(nb, jb, 
                                     c_one, dX(i),   nb,
                                            dH(i,j), ldda);
                    // copy to dY to compute dW
                    magma_zcopymatrix( nb,jb, dH(i,j), ldda, dY(i), nb );
                }
                // * insert event back to keep track
                magma_event_record( event[0], stream[0] );
                magma_event_record( event[1], stream[1] );
                // * make sure they are done
                magma_queue_wait_event( stream[0], event[1] );
                magma_queue_wait_event( stream[1], event[0] );
                for (int i=1; i<j; i++)
                {
                    // W(i) += .5*H(i,j)
                    magmablasSetKernelStream(stream[(i-1)%2]);
                    magmablas_zgeadd(nb, jb, 
                                    -c_half, dX(i), nb,
                                             dY(i), nb);
                    // transpose W for calling zher2k
                    magmablas_ztranspose( nb,jb, dY(i), nb, dW(i), lddw );

                    // > H(i,j) += T(i,i-1) * L(j,i-1)', X
                    if (i > 1) // if i==1, then L(j,i-1) = 0
                    {
                        // W(i+1) = T(i,i-1)*L(j,i-1)'
                        magma_zgemm(MagmaNoTrans, MagmaConjTrans,
                                    nb, jb, nb,
                                    c_one, dT(i,i-1), ldda,
                                           dL(j,i-1), ldda,
                                    c_one, dH(i,j),   ldda);
                    }
                }
                trace_gpu_end( 0,0 );
                trace_gpu_end( 0,1 );
                // * insert event back to keep track
                magma_event_record( event[0], stream[0] );
                magma_event_record( event[1], stream[1] );
                // * make sure they are done
                magma_queue_wait_event( stream[0], event[1] );
                magmablasSetKernelStream(stream[0]);

                // compute T(j, j) = A(j,j) - L(j,1:j)*H(1:j,j) (where T is A in memory)
                trace_gpu_start( 0, 0, "her2k", "compTjj" );
                if (j > 1)
                magma_zher2k(MagmaLower, MagmaNoTrans,
                             jb, (j-1)*nb,
                             c_mone, dL(j,1), ldda,
                                     dW(1),   lddw,
                             d_one,  dT(j,j), ldda);
                magmablas_zsymmetrize(MagmaLower, jb, dT(j,j), ldda);
                trace_gpu_end( 0,0 );
                // > Compute T(j,j) - L(j,j)^-1 T(j,j) L^(j,j)^-T
                trace_gpu_start( 0, 0, "trsm", "compTjj" );
                if (j > 0) // if j == 1, then L(j,j) = I
                {
                    magma_ztrsm( MagmaLeft, MagmaLower, MagmaNoTrans, MagmaUnit,
                                 jb, jb,
                                 c_one, dL(j,j), ldda,
                                        dT(j,j), ldda);
                    magma_ztrsm( MagmaRight, MagmaLower, MagmaConjTrans, MagmaUnit,
                                 jb, jb,
                                 c_one, dL(j,j), ldda,
                                        dT(j,j), ldda);
                }
                trace_gpu_end( 0,0 );
                if (j < (n+nb-1)/nb-1)
                {
                    // ** Panel + Update **
                    // compute H(j,j)
                    // > H(j,j) = T(j,j)*L(j,j)'
                    //   H(0,0) is not needed since it is multiplied with L(j+1:n,0)
                    trace_gpu_start( 0, 0, "trmm", "compHjj" );
                    if (j >= 1)
                    {
                        magma_zgemm(MagmaNoTrans, MagmaConjTrans,
                                    jb, jb, nb,
                                    c_one,  dT(j,j), ldda,
                                            dL(j,j), ldda,
                                    c_zero, dH(j,j), ldda);
                        if (j >= 2)
                        {
                            // > H(j,j) += T(j,j-1)*L(j,j-1)
                            magma_zgemm(MagmaNoTrans, MagmaConjTrans,
                                        jb, jb, nb,
                                        c_one, dT(j,j-1), ldda,
                                               dL(j,j-1), ldda,
                                        c_one, dH(j,j),   ldda);
                        }
                    }
                    trace_gpu_end( 0,0 );
                    // extract L(:, j+1)
                    trace_gpu_start( 0, 0, "gemm", "compLj" );
                    magma_zgemm(MagmaNoTrans, MagmaNoTrans,
                                n-(j+1)*nb, jb, (j-1)*nb+jb,
                                c_mone, dL(j+1,1), ldda,
                                        dH(  1,j), ldda,
                                c_one,  dA(j+1,j), ldda);
                    trace_gpu_end( 0,0 );

                    // panel factorization
                    int ib = n-(j+1)*nb;
                    if (cpu_panel || j < 2) {
                        // copy panel to CPU
                        magma_zgetmatrix_async( ib, jb,
                                                dA(j+1,j), ldda,
                                                 A(j+1,j), lda, stream[0] );
                        // panel factorization on CPU
                        magma_queue_sync( stream[0] );
                        trace_cpu_start( 0, "getrf", "getrf" );
                        lapackf77_zgetrf( &ib, &jb, A(j+1,j), &lda, &ipiv[(1+j)*nb], &iinfo);
                        if (iinfo != 0) printf( " zgetrf failed with %d\n",iinfo );
                        trace_cpu_end( 0 );
                        // copy to GPU (all columns, not just L part)
                        magma_zsetmatrix_async( ib, jb, A(j+1,j), lda, dA(j+1,j), ldda,
                                                stream[0]);
                    } else {
                        //#define USE_BATCHED_ZGETRF
                        #ifdef USE_BATCHED_ZGETRF
                        //dA_array[0] = dA(j+1,j);
                        //dipiv_array[0] = dipiv_magma;
                        zset_pointer(dA_array, dA(j+1,j), ldda, 0, 0, 0, 1);
                        set_ipointer(dipiv_array, dipiv_magma, 1, 0, 0, min(ib,jb), 1);
                        iinfo = magma_zgetrf_batched( ib, jb, dA_array, ldda, dipiv_array, dinfo_magma, 1);
                        // copy ipiv to CPU since permu vector is generated on CPU, for now..
                        magma_igetvector_async( min(ib,jb), dipiv_magma, 1, &ipiv[(1+j)*nb], 1, stream[0]);
                        magma_queue_sync( stream[0] );
                        #else
                        magma_zgetf2_gpu( ib, jb, dA(j+1,j), ldda, &ipiv[(1+j)*nb], &iinfo);
                        #endif
                    }
                    // save L(j+1,j+1), and make it to unit-lower triangular
                    magma_zcopymatrix( min(ib,jb),min(ib,jb), dA(j+1,j), ldda, dL(j+1,j+1), ldda );
                    magmablas_zlaset(MagmaUpper, min(ib,jb),min(ib,jb), c_zero,c_one, dL(j+1,j+1),ldda);
                    // extract T(j+1,j)
                    magmablas_zlaset(MagmaLower, min(ib,jb)-1,jb-1, c_zero,c_zero, dT(j+1,j)+1,ldda);
                    if (j > 0)
                    magma_ztrsm( MagmaRight, MagmaLower, MagmaConjTrans, MagmaUnit,
                                 min(ib,jb), jb,
                                 c_one, dL(j,j), ldda,
                                        dT(j+1,j), ldda);

                    // apply pivot back
                    trace_gpu_start( 0, 0, "permute", "permute" );
                    magmablas_zlaswpx( j*nb, dL(j+1, 1), 1, ldda, 
                                       1, min(jb,ib), &ipiv[(j+1)*nb], 1 );
                    // symmetric pivot
                    {
                        for (int ii=0; ii<min(jb,ib); ii++) {
                            int piv = perm[ipiv[(j+1)*nb+ii]-1];
                            perm[ipiv[(j+1)*nb+ii]-1] = perm[ii];
                            perm[ii] = piv;
                        }
                        int count = 0;
                        for (int ii=0; ii<n; ii++) {
                            if (perm[ii] != ii) {
                                rows[2*count] = perm[ii];
                                rows[2*count+1] = ii;
                                count ++;
                            }
                        }
                        magma_isetvector_async( 2*count, rows, 1, drows, 1, stream[0]);
                        magmablas_zlacpy_sym_in(MagmaLower, n-(j+1)*nb, count, drows, dperm, dA(j+1,j+1),ldda, dH(0,0),ldda);
                        magmablas_zlacpy_sym_out(MagmaLower, n-(j+1)*nb, count, drows, dperm, dH(0,0),ldda, dA(j+1,j+1),ldda);

                        // reset perm
                        for (int ii=0; ii<count; ii++) perm[rows[2*ii+1]] = rows[2*ii+1];
                        //for (int k=0; k<n; k++) printf( "%d ",perm[k] );
                        //printf( "\n" );

                    }
                    for (int k=(1+j)*nb; k<(1+j)*nb+min(jb,ib); k++) ipiv[k] += (j+1)*nb;
                    trace_gpu_end( 0,0 );
                }
            }
            // copy back to CPU
            for (int j=0; j<(n+nb-1)/nb; j++)
            {
                int jb = min(nb, n-j*nb);
                //#define COPY_BACK_BY_BLOCK_COL
                #if defined(COPY_BACK_BY_BLOCK_COL)
                magma_zgetmatrix_async( n-j*nb, jb, dA(j,j), ldda, A(j,j), lda, stream[0] );
                #else
                // copy T
                magma_zgetmatrix_async( jb, jb, dT(j,j), ldda, A(j,j), lda, stream[0] );
                if (j < (n+nb-1)/nb-1)
                {
                    // copy L
                    int jb2 = min(nb, n-(j+1)*nb);
                    magmablas_zlacpy( MagmaLower, jb2-1,jb2-1, dL(j+1,j+1)+1, ldda, dA(j+1,j)+1, ldda );
                    magma_zgetmatrix_async( n-j*nb-jb, jb, dA(j+1,j), ldda, A(j+1,j), lda, stream[0] );
                }
                #endif
            }
        }
    }
    
    for (int i=0; i<num_streams; i++) {
        magma_queue_sync( stream[i] );
        magma_queue_destroy( stream[i] );
        magma_event_destroy( event[i] );
    }
    magmablasSetKernelStream( orig_stream );
    trace_finalize( "zhetrf.svg","trace.css" );

    magma_free(dA_array);
    magma_free(dipiv_array);
    magma_free( dipiv_magma);
    magma_free( dinfo_magma );
    magma_free(dperm);
    magma_free(drows);
    magma_free_pinned(rows);
    magma_free_cpu(perm);

    magma_free( work );
    magma_free( dH );
    magma_free( dL );
    magma_free( dX );
    magma_free( dY );
    magma_free( dW );
    
    return *info;
} /* magma_zhetrf_aasen */
