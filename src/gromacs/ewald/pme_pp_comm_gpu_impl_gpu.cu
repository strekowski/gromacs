/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright 2019- The GROMACS Authors
 * and the project initiators Erik Lindahl, Berk Hess and David van der Spoel.
 * Consult the AUTHORS/COPYING files and https://www.gromacs.org for details.
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
 * https://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at https://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out https://www.gromacs.org.
 */
/*! \internal \file
 *
 * \brief Implements PME-PP communication using CUDA
 *
 *
 * \author Alan Gray <alang@nvidia.com>
 *
 * \ingroup module_ewald
 */
#include "gmxpre.h"

#include "pme_pp_comm_gpu_impl.h"

#include "config.h"

#include "gromacs/gpu_utils/cudautils.cuh"
#include "gromacs/gpu_utils/device_context.h"
#include "gromacs/gpu_utils/device_stream.h"
#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/gpu_utils/gpueventsynchronizer.h"
#include "gromacs/gpu_utils/typecasts.cuh"

#include "pme_pp_communication.h"

namespace gmx
{

void PmePpCommGpu::Impl::sendCoordinatesToPmePeerToPeer(Float3*               sendPtr,
                                                        int                   sendSize,
                                                        GpuEventSynchronizer* coordinatesReadyOnDeviceEvent)
{
    // ensure stream waits until coordinate data is available on device
    coordinatesReadyOnDeviceEvent->enqueueWaitEvent(pmePpCommStream_);

    cudaError_t stat = cudaMemcpyAsync(remotePmeXBuffer_,
                                       sendPtr,
                                       sendSize * DIM * sizeof(float),
                                       cudaMemcpyDefault,
                                       pmePpCommStream_.stream());
    CU_RET_ERR(stat, "cudaMemcpyAsync on Send to PME CUDA direct data transfer failed");

#if GMX_MPI
    // Record and send event to allow PME task to sync to above transfer before commencing force calculations
    pmeCoordinatesSynchronizer_.markEvent(pmePpCommStream_);
    GpuEventSynchronizer* pmeSync = &pmeCoordinatesSynchronizer_;
    // NOLINTNEXTLINE(bugprone-sizeof-expression)
    MPI_Send(&pmeSync, sizeof(GpuEventSynchronizer*), MPI_BYTE, pmeRank_, 0, comm_);
#endif
}

} // namespace gmx
