/*
    Solve the Poisson problem
        u_{xx} = f(x)   x \in [a, b]
    with 
        u(a) = alpha, u(b) = beta
    using Jacobi iterations and MPI.

    Note that the stencil has the following layout
        [* 0, 1, 2, ... n-2, n-1, *]
*/

// MPI Library
#include "mpi.h"

// Standard IO libraries
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <math.h>

int main(int argc, char* argv[])
{
    int num_procs, rank, tag;
    MPI_Status status;
    MPI_Request request;

    // Problem paramters
    double const alpha = 0.0, beta = 3.0, a = 0.0, b = 1.0;

    // Numerical parameters
    int const MAX_ITERATIONS = 10000, PRINT_INTERVAL = 1000;
    int N, num_points, points_per_proc, start_index, end_index;
    double x, dx, tolerance, du_max, du_max_proc;

    bool serial_output = false;

    // Work arrays
    double *u, *u_old, *f;

    // IO
    FILE *fp;
    char file_name[20];

    // Initialize MPI
    MPI_Init(&argc, &argv);

    // Get total number of processes and this processes' rank
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Rank 0 will ask for number of points
    if (rank == 0)
    {
        printf("How many points to use?\n");
        scanf("%d", &num_points);
    }
    // Broadcast the number of points
    MPI_Bcast(&num_points, 1, MPI_INTEGER, 0, MPI_COMM_WORLD);

    // Compute dx and tolerance based on this
    dx = (b - a) / ((double)(num_points + 1));
    tolerance = 0.1 * pow(dx, 2);

    // Determine how many points to handle with each proc
    points_per_proc = (num_points + num_procs - 1) / num_procs;
    // Only print out the number of points per proc by rank 0
    if (rank == 0)
        printf("Points per proc = %d\n.", points_per_proc);

    // Determine start and end indices for this rank's points
    start_index = rank * points_per_proc;
    end_index = (int)fmin((rank + 1) * points_per_proc, num_points) - 1;

    // Diagnostic - Print the intervals handled by each rank
    printf("Rank %d - (%d, %d)\n", rank, start_index, end_index);

    // Determine start and end indices for this rank's points for looping.
    // Actual size of the work arrays include boundary points
    start_index = rank * points_per_proc + 1;
    end_index = (int)fmin((rank + 1) * points_per_proc, num_points);

    // Allocate memory for work space
    u = malloc((points_per_proc + 2) * sizeof(double));
    u_old = malloc((points_per_proc + 2) * sizeof(double));
    f = malloc((points_per_proc + 2) * sizeof(double));

    // Initialize arrays
    for (int i = start_index; i <= end_index; ++i)
    {
        x = dx * (double) i;
        f[i] = exp(x);                     // RHS function
        u[i] = alpha + x * (beta - alpha); // Initial guess
    }

    // If rank is keeping track of a boundary we should set that
    if (rank == 0)
        // Equivalent to u[0]
        u[start_index - 1] = alpha;
    if (rank == num_procs - 1)
        // Equivalent to u[num_points + 2 - 1]
        u[end_index + 1] = beta;

    /* Jacobi Iterations */
    while (N < MAX_ITERATIONS)
    {
        // Copy u into u_old
        for (int i = 0; i < points_per_proc + 2; ++i)
            u_old[i] = u[i];

        /* Fill in boundary data */
        // Here we are using a tag = 1 for left going communication and tag = 2 for right
        // Send and receive data to fill in overlaps
        if (rank > 0)
        {
            // Send left endpoint value to process to the left - non-blocking sends
            MPI_Isend(&u_old[0], 1, MPI_DOUBLE_PRECISION, rank - 1, 1, MPI_COMM_WORLD, &request);
        }
        if (rank < num_procs - 1)
        {
            // Send right endpoint value to process to the right - non-blocking sends
            MPI_Isend(&u_old[end_index + 1], 1, MPI_DOUBLE_PRECISION, rank + 1, 2, MPI_COMM_WORLD, &request);
        }

        // Accept incoming left and right endpoint values from other tasks - blocking receives
        if (rank < num_procs - 1)
            // Communication from rank + 1
            MPI_Recv(&u_old[end_index + 1], 1, MPI_DOUBLE_PRECISION, rank + 1, 1, MPI_COMM_WORLD, &status);
        if (rank > 0)
            MPI_Recv(&u_old[0], 1, MPI_DOUBLE_PRECISION, rank - 1, 2, MPI_COMM_WORLD, &status);

        /* Apply Jacobi */
        du_max_proc = 0.0;
        for (int i = 1; i < points_per_proc + 1; ++i)
        {
            u[i] = 0.5 * (u_old[i-1] + u_old[i+1] - pow(dx, 2) * f[i]);
            du_max_proc = fmax(du_max_proc, fabs(u[i] - u_old[i]));
        }

        // Find global maximum change in solution - acts as an implicit barrier
        MPI_Allreduce(&du_max_proc, &du_max, 1, MPI_DOUBLE_PRECISION, MPI_MAX, MPI_COMM_WORLD);

        // Periodically report progress
        if (rank == 0)
            if (N%PRINT_INTERVAL == 0)
                printf("After %d iterations, du_max = %f.\n", N, du_max);

        // All processes have the same du_max and should check for convergence
        if (du_max < tolerance)
            break;
        N++;
    }


    printf("Rank %d finished after %d iterations, du_max = %f.\n",
            rank, N, du_max);

    // Output Results
    // Check for failure
    if (N >= MAX_ITERATIONS)
    {
        if (rank == 0)
        {
            printf("*** Jacobi failed to converge!\n");
            printf("***   Reached du_max = %f\n", du_max);
            printf("***   Tolerance = %f\n", tolerance);
        }
        MPI_Finalize();
        return 1;
    }

    // Synchronize here before output
    MPI_Barrier(MPI_COMM_WORLD);

    // Demonstration of two approaches to writing out files:
    if (serial_output)
    {
        // Each rank will open the same file for writing in turn waiting for
        // the previous rank to tell it to go.
        if (rank == 0)
        {
            // Setup file for writing and let rank + 1 to go
            fp = fopen("jacobi_mpi.txt", "w");
            fprintf(fp, "%f %f", 0.0, u[0]);

            for (int i = 1; i < end_index; ++i)
            {
                x = (double)i * dx;
                fprintf(fp, "%f %f", x, u[i]);
            }

            fclose(fp);

            // Notify next rank
            if (num_procs > 1)
                MPI_Send(MPI_BOTTOM, 0, MPI_INTEGER, 1, 4, MPI_COMM_WORLD);
        }
        else
        {
            // Wait to go for the previous rank, write out, and let the next rank know to go.
            MPI_Recv(MPI_BOTTOM, 0, MPI_INTEGER, rank - 1, 4, MPI_COMM_WORLD, &status);

            // Begin writing out
            fp = fopen("jacobi_mpi.txt", "w");
            for (int i = 1; i < end_index; ++i)
            {
                x = (double)i * dx;
                fprintf(fp, "%f %f", x, u[i]);
            }

            // This is the last process, write out boundary
            if (rank == num_procs - 1)
                fprintf(fp, "%f %f", b, u[end_index + 1]);

            fclose(fp);

            // Send signal to next rank if necessary
            if (rank < num_procs - 1)
                MPI_Send(MPI_BOTTOM, 0, MPI_INTEGER, rank + 1, 4, MPI_COMM_WORLD);
        }
    }
    else
    {
        // Each rank writes out to it's own file and post-processing will handle opening
        // up all the files - rank determines the file names
        sprintf(file_name, "jacobi_%d.txt", rank);
        // printf("%s", file_name);
        // fp = fopen(strcat("jacobi_", ".txt"), "w");
        // fp = fopen(strcat(strcat("jacobi_", itoa(rank)), ".txt"), 'w');
        fp = fopen(file_name, "w");

        if (rank == 0)
            fprintf(fp, "%f %f", 0.0, u[0]);

        for (int i = 1; i < end_index; ++i)
        {
            x = (double)i * dx;
            fprintf(fp, "%f %f", x, u[i]);
        }

        if (rank == num_procs - 1)
            fprintf(fp, "%f %f", b, u[end_index + 1]);

        fclose(fp);
    }

    MPI_Finalize();

    return 0;
}
