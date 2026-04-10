/*
 * trans.c - Matrix transpose B = A^T
 *
 * Each transpose function must have a prototype of the form:
 * void trans(int M, int N, int A[N][M], int B[M][N]);
 *
 * A transpose function is evaluated by counting the number of misses
 * on a 1KB direct mapped cache with a block size of 32 bytes.
 */
#include <stdio.h>
#include "cachelab.h"

int is_transpose(int M, int N, int A[N][M], int B[M][N]);

static void transpose_32(int M, int N, int A[N][M], int B[M][N]);
static void transpose_64(int M, int N, int A[N][M], int B[M][N]);
static void transpose_generic(int M, int N, int A[N][M], int B[M][N]);

/*
 * transpose_submit - This is the solution transpose function that you
 *     will be graded on for Part B of the assignment. Do not change
 *     the description string "Transpose submission", as the driver
 *     searches for that string to identify the transpose function to
 *     be graded.
 */
char transpose_submit_desc[] = "Transpose submission";
void transpose_submit(int M, int N, int A[N][M], int B[M][N])
{
    if (M == 32 && N == 32) {
        transpose_32(M, N, A, B);
    } else if (M == 64 && N == 64) {
        transpose_64(M, N, A, B);
    } else {
        transpose_generic(M, N, A, B);
    }
}

static void transpose_32(int M, int N, int A[N][M], int B[M][N])
{
    int i, j, k;
    int a0, a1, a2, a3, a4, a5, a6, a7;

    for (i = 0; i < N; i += 8) {
        for (j = 0; j < M; j += 8) {
            for (k = i; k < i + 8; ++k) {
                a0 = A[k][j];
                a1 = A[k][j + 1];
                a2 = A[k][j + 2];
                a3 = A[k][j + 3];
                a4 = A[k][j + 4];
                a5 = A[k][j + 5];
                a6 = A[k][j + 6];
                a7 = A[k][j + 7];

                B[j][k] = a0;
                B[j + 1][k] = a1;
                B[j + 2][k] = a2;
                B[j + 3][k] = a3;
                B[j + 4][k] = a4;
                B[j + 5][k] = a5;
                B[j + 6][k] = a6;
                B[j + 7][k] = a7;
            }
        }
    }
}

static void transpose_64(int M, int N, int A[N][M], int B[M][N])
{
    int i, j, k;
    int a0, a1, a2, a3, a4, a5, a6, a7;

    for (i = 0; i < N; i += 8) {
        for (j = 0; j < M; j += 8) {
            for (k = 0; k < 4; ++k) {
                a0 = A[i + k][j];
                a1 = A[i + k][j + 1];
                a2 = A[i + k][j + 2];
                a3 = A[i + k][j + 3];
                a4 = A[i + k][j + 4];
                a5 = A[i + k][j + 5];
                a6 = A[i + k][j + 6];
                a7 = A[i + k][j + 7];

                B[j][i + k] = a0;
                B[j + 1][i + k] = a1;
                B[j + 2][i + k] = a2;
                B[j + 3][i + k] = a3;
                B[j][i + k + 4] = a4;
                B[j + 1][i + k + 4] = a5;
                B[j + 2][i + k + 4] = a6;
                B[j + 3][i + k + 4] = a7;
            }

            for (k = 0; k < 4; ++k) {
                a0 = B[j + k][i + 4];
                a1 = B[j + k][i + 5];
                a2 = B[j + k][i + 6];
                a3 = B[j + k][i + 7];

                a4 = A[i + 4][j + k];
                a5 = A[i + 5][j + k];
                a6 = A[i + 6][j + k];
                a7 = A[i + 7][j + k];

                B[j + k][i + 4] = a4;
                B[j + k][i + 5] = a5;
                B[j + k][i + 6] = a6;
                B[j + k][i + 7] = a7;

                B[j + 4 + k][i] = a0;
                B[j + 4 + k][i + 1] = a1;
                B[j + 4 + k][i + 2] = a2;
                B[j + 4 + k][i + 3] = a3;
            }

            for (k = 0; k < 4; ++k) {
                a0 = A[i + 4 + k][j + 4];
                a1 = A[i + 4 + k][j + 5];
                a2 = A[i + 4 + k][j + 6];
                a3 = A[i + 4 + k][j + 7];

                B[j + 4][i + 4 + k] = a0;
                B[j + 5][i + 4 + k] = a1;
                B[j + 6][i + 4 + k] = a2;
                B[j + 7][i + 4 + k] = a3;
            }
        }
    }
}

static void transpose_generic(int M, int N, int A[N][M], int B[M][N])
{
    int i, j, k, l;

    for (i = 0; i < N; i += 16) {
        for (j = 0; j < M; j += 16) {
            for (k = i; k < N && k < i + 16; ++k) {
                for (l = j; l < M && l < j + 16; ++l) {
                    B[l][k] = A[k][l];
                }
            }
        }
    }
}

/*
 * trans - A simple baseline transpose function, not optimized for the cache.
 */
char trans_desc[] = "Simple row-wise scan transpose";
void trans(int M, int N, int A[N][M], int B[M][N])
{
    int i, j, tmp;

    for (i = 0; i < N; i++) {
        for (j = 0; j < M; j++) {
            tmp = A[i][j];
            B[j][i] = tmp;
        }
    }
}

/*
 * registerFunctions - This function registers your transpose
 *     functions with the driver.  At runtime, the driver will
 *     evaluate each of the registered functions and summarize their
 *     performance. This is a handy way to experiment with different
 *     transpose strategies.
 */
void registerFunctions()
{
    registerTransFunction(transpose_submit, transpose_submit_desc);
    registerTransFunction(trans, trans_desc);
}

/*
 * is_transpose - This helper function checks if B is the transpose of
 *     A. You can check the correctness of your transpose by calling
 *     it before returning from the transpose function.
 */
int is_transpose(int M, int N, int A[N][M], int B[M][N])
{
    int i, j;

    for (i = 0; i < N; i++) {
        for (j = 0; j < M; ++j) {
            if (A[i][j] != B[j][i]) {
                return 0;
            }
        }
    }
    return 1;
}
