#include "openmc/random_ray/linear_source_domain.h"

#include "openmc/cell.h"
#include "openmc/geometry.h"
#include "openmc/material.h"
#include "openmc/message_passing.h"
#include "openmc/mgxs_interface.h"
#include "openmc/output.h"
#include "openmc/plot.h"
#include "openmc/random_ray/random_ray.h"
#include "openmc/simulation.h"
#include "openmc/tallies/filter.h"
#include "openmc/tallies/tally.h"
#include "openmc/tallies/tally_scoring.h"
#include "openmc/timer.h"

#include <cstdio>

namespace openmc {

//==============================================================================
// LinearSourceDomain implementation
//==============================================================================

LinearSourceDomain::LinearSourceDomain() : FlatSourceDomain()
{
    flux_x_new_.assign(n_source_elements_, 0.0);
    flux_x_old_.assign(n_source_elements_, 0.0);
    source_x_.assign(n_source_elements_, 0.0); 
    flux_y_new_.assign(n_source_elements_, 0.0);
    flux_y_old_.assign(n_source_elements_, 0.0);
    source_y_.assign(n_source_elements_, 0.0); 
    flux_z_new_.assign(n_source_elements_, 0.0);
    flux_z_old_.assign(n_source_elements_, 0.0);
    source_z_.assign(n_source_elements_, 0.0); 

    centroid_.assign(n_source_regions_ * 3, nan(""));
    centroid_t_.assign(n_source_regions_ * 3, 0.0);
    mom_matrix_.assign(n_source_regions_ * 6, 0.0);
    mom_matrix_t_.assign(n_source_regions_ * 6, 0.0);
}

void LinearSourceDomain::batch_reset()
{
    FlatSourceDomain::batch_reset();
    parallel_fill<float>(flux_x_new_, 0.0f);
    parallel_fill<float>(flux_y_new_, 0.0f);
    parallel_fill<float>(flux_z_new_, 0.0f);
}

// void LinearSourceDomain::accumulate_iteration_flux()
// {
//     // Fully reimplement this function for linear source
//     // ...
// }

// Calculate source gradients by inverting the moment matrix
void moment_matrix_inversion(const std::vector<double>& mom_matrix_, int sr, double& invM1, double& invM2, 
                        double& invM3, double& invM4, double& invM5, double& invM6)
{
      int64_t midx = sr * 6;
      double det = mom_matrix_[midx + 0] * (mom_matrix_[midx + 3] * mom_matrix_[midx + 5] - mom_matrix_[midx + 4] * mom_matrix_[midx + 4]) -
                  mom_matrix_[midx + 3] * mom_matrix_[midx + 2] * mom_matrix_[midx + 2] -
                  mom_matrix_[midx + 5] * mom_matrix_[midx + 1] * mom_matrix_[midx + 1] +
                  2 * mom_matrix_[midx + 1] * mom_matrix_[midx + 2] * mom_matrix_[midx + 4];
      if ( abs(det) > 0.0 ) {  //1E-10
        double one_det = 1.0 / det;
        invM1 = one_det * (mom_matrix_[midx + 3] * mom_matrix_[midx + 5] - mom_matrix_[midx + 4] * mom_matrix_[midx + 4]);
        invM2 = one_det * (mom_matrix_[midx + 2] * mom_matrix_[midx + 4] - mom_matrix_[midx + 1] * mom_matrix_[midx + 5]);
        invM3 = one_det * (mom_matrix_[midx + 1] * mom_matrix_[midx + 4] - mom_matrix_[midx + 3] * mom_matrix_[midx + 2]);
        invM4 = one_det * (mom_matrix_[midx + 0] * mom_matrix_[midx + 5] - mom_matrix_[midx + 2] * mom_matrix_[midx + 2]);
        invM5 = one_det * (mom_matrix_[midx + 1] * mom_matrix_[midx + 2] - mom_matrix_[midx + 0] * mom_matrix_[midx + 4]);
        invM6 = one_det * (mom_matrix_[midx + 0] * mom_matrix_[midx + 3] - mom_matrix_[midx + 1] * mom_matrix_[midx + 1]);
      } else {
        invM1 = 0.0;
        invM2 = 0.0;
        invM3 = 0.0;
        invM4 = 0.0;
        invM5 = 0.0;
        invM6 = 0.0;
        det = 1.0;
      }

}

void LinearSourceDomain::update_neutron_source(double k_eff)
{
simulation::time_update_src.start();

  double inverse_k_eff = 1.0 / k_eff;

  // Temperature and angle indices, if using multiple temperature
  // data sets and/or anisotropic data sets.
  // TODO: Currently assumes we are only using single temp/single
  // angle data.
  const int t = 0;
  const int a = 0;
  //double det;

  #pragma omp parallel for
    for (int sr = 0; sr < n_source_regions_; sr++) {
      int material = material_[sr];
      double invM1, invM2, invM3, invM4, invM5, invM6; 
      moment_matrix_inversion(mom_matrix_, sr, invM1, invM2, invM3, invM4, invM5, invM6);
      
      for (int e_out = 0; e_out < negroups_; e_out++) {
        float sigma_t = data::mg.macro_xs_[material].get_xs(
          MgxsType::TOTAL, e_out, nullptr, nullptr, nullptr, t, a);

        float scatter_source = 0.0f;
        float x_scatter = 0.0f;
        float y_scatter = 0.0f;
        float z_scatter = 0.0f;
        float x_fission = 0.0f;
        float y_fission = 0.0f;
        float z_fission = 0.0f;

        for (int e_in = 0; e_in < negroups_; e_in++) {
          float scalar_flux = scalar_flux_old_[sr * negroups_ + e_in];
          float flux_x = flux_x_old_[sr * negroups_ + e_in];
          float flux_y = flux_y_old_[sr * negroups_ + e_in];
          float flux_z = flux_z_old_[sr * negroups_ + e_in];
          float sigma_s = data::mg.macro_xs_[material].get_xs(
            MgxsType::NU_SCATTER, e_in, &e_out, nullptr, nullptr, t, a);
          float nu_sigma_f = data::mg.macro_xs_[material].get_xs(
            MgxsType::NU_FISSION, e_in, nullptr, nullptr, nullptr, t, a);
          float chi = data::mg.macro_xs_[material].get_xs(
            MgxsType::CHI_PROMPT, e_in, &e_out, nullptr, nullptr, t, a);
          scatter_source += sigma_s * scalar_flux;
          // Calculate scattering source for higher order scattering
          x_scatter += sigma_s * flux_x ;
          y_scatter += sigma_s * flux_y ;
          z_scatter += sigma_s * flux_z;
          x_fission += nu_sigma_f * flux_x * chi;
          y_fission += nu_sigma_f * flux_y * chi;
          z_fission += nu_sigma_f * flux_z * chi;
        }


        // fission_source *= inverse_k_eff;
        float new_isotropic_source = (scatter_source) / sigma_t;
        source_[sr * negroups_ + e_out] = new_isotropic_source;

        if (simulation::current_batch > 2) {

          x_fission *= inverse_k_eff;
          y_fission *= inverse_k_eff;
          z_fission *= inverse_k_eff;

          float x_source = (x_scatter + x_fission) / sigma_t;
          float y_source = (y_scatter + y_fission) / sigma_t;
          float z_source = (z_scatter + z_fission) / sigma_t;

          float new_source_x = invM1 * x_source + invM2 * y_source + invM3 * z_source;
          float new_source_y = invM2 * x_source + invM4 * y_source + invM5 * z_source;
          float new_source_z = invM3 * x_source + invM5 * y_source + invM6 * z_source;
          source_x_[sr * negroups_ + e_out] = new_source_x;
          source_y_[sr * negroups_ + e_out] = new_source_y;
          source_z_[sr * negroups_ + e_out] = new_source_z;

        } 
      }
  }

   if (settings::run_mode == RunMode::EIGENVALUE) {
          #pragma omp parallel for
            for (int sr = 0; sr < n_source_regions_; sr++) {
              int material = material_[sr];

              for (int e_out = 0; e_out < negroups_; e_out++) {
                float sigma_t = data::mg.macro_xs_[material].get_xs(
                  MgxsType::TOTAL, e_out, nullptr, nullptr, nullptr, t, a);
                float fission_source = 0.0f;

                for (int e_in = 0; e_in < negroups_; e_in++) {
                  float scalar_flux = scalar_flux_old_[sr * negroups_ + e_in];
                  float nu_sigma_f = data::mg.macro_xs_[material].get_xs(
                    MgxsType::NU_FISSION, e_in, nullptr, nullptr, nullptr, t, a);
                  float chi = data::mg.macro_xs_[material].get_xs(
                    MgxsType::CHI_PROMPT, e_in, &e_out, nullptr, nullptr, t, a);
                  fission_source += nu_sigma_f * scalar_flux * chi;
                }
                source_[sr * negroups_ + e_out] +=
                  fission_source * inverse_k_eff / sigma_t;
              }
            } 
            }
            else {
                // Add external source if in fixed source mode
                #pragma omp parallel for
                    for (int se = 0; se < n_source_elements_; se++) {
                      source_[se] += external_source_[se];
                    }
            }

  simulation::time_update_src.stop();
}

void LinearSourceDomain::normalize_scalar_flux_and_volumes(
  double total_active_distance_per_iteration)
{
  float normalization_factor = 1.0 / total_active_distance_per_iteration;
  double volume_normalization_factor =
    1.0 / (total_active_distance_per_iteration * simulation::current_batch);

// Normalize flux to total distance travelled by all rays this iteration
#pragma omp parallel for
  for (int64_t e = 0; e < scalar_flux_new_.size(); e++) {
    scalar_flux_new_[e] *= normalization_factor;
    flux_x_new_[e] *= normalization_factor;
    flux_y_new_[e] *= normalization_factor;
    flux_z_new_[e] *= normalization_factor;
  }

// Accumulate cell-wise ray length tallies collected this iteration, then
// update the simulation-averaged cell-wise volume estimates
#pragma omp parallel for
  for (int64_t sr = 0; sr < n_source_regions_; sr++) {
    volume_t_[sr] += volume_[sr];
    volume_[sr] = volume_t_[sr] * volume_normalization_factor;
  }
}


int64_t LinearSourceDomain::add_source_to_scalar_flux()
{
  int64_t n_hits = 0;

  // Temperature and angle indices, if using multiple temperature
  // data sets and/or anisotropic data sets.
  // TODO: Currently assumes we are only using single temp/single
  // angle data.
  const int t = 0;
  const int a = 0;

#pragma omp parallel for reduction(+ : n_hits)
  for (int sr = 0; sr < n_source_regions_; sr++) {

    double volume = volume_[sr];
    double volume_tracks = volume_t_[sr];
    double invvol = 1.0f / volume_tracks;
    int material = material_[sr];
    int64_t didx   = sr * 3;
    int64_t midx   = sr * 6;

    // Check if this cell was hit this iteration
    int was_cell_hit = was_hit_[sr];
    if (was_cell_hit) {
      n_hits++;
    }

    for (int g = 0; g < negroups_; g++) {
      int64_t idx = (sr * negroups_) + g;
      // There are three scenarios we need to consider:
      if (was_cell_hit) {
        // 1. If the FSR was hit this iteration, then the new flux is equal to
        // the flat source from the previous iteration plus the contributions
        // from rays passing through the source region (computed during the
        // transport sweep)
        scalar_flux_new_[idx] /= volume;
        scalar_flux_new_[idx] += source_[idx];
        flux_x_new_[idx] /= volume ;
        flux_y_new_[idx] /= volume ;
        flux_z_new_[idx] /= volume ;
        for (int i = 0; i < 3; i++) { // Update centroids
          centroid_[didx + i] = centroid_t_[didx + i] * invvol;
        }
        for (int i = 0; i < 6; i++) { // Update spatial moments
          mom_matrix_[midx + i] = mom_matrix_t_[midx + i] * invvol;
        }
      } else if (volume > 0.0) {
        // 2. If the FSR was not hit this iteration, but has been hit some
        // previous iteration, then we simply set the new scalar flux to be
        // equal to the contribution from the flat source alone.
        scalar_flux_new_[idx] = source_[idx];
        flux_x_new_[idx] /= volume ;
        flux_y_new_[idx] /= volume ;
        flux_z_new_[idx] /= volume ;
        for (int i = 0; i < 3; i++) { // Update centroids
          centroid_[didx + i] = centroid_t_[didx + i] * invvol;
        }
        for (int i = 0; i < 6; i++) { // Update spatial moments
          mom_matrix_[midx + i] = mom_matrix_t_[midx + i] * invvol;
        }
      } else {
        // If the FSR was not hit this iteration, and it has never been hit in
        // any iteration (i.e., volume is zero), then we want to set this to 0
        // to avoid dividing anything by a zero volume.
        scalar_flux_new_[idx] = 0.0f;
        flux_x_new_[idx] = 0.0f ;
        flux_y_new_[idx] = 0.0f ;
        flux_z_new_[idx] = 0.0f ;
      }
    }
  }

  return n_hits;
}

// double LinearSourceDomain::compute_k_eff(double k_eff_old) const
// {
//   FlatSourceDomain::compute_k_eff(k_eff_old);
// }

// void LinearSourceDomain::random_ray_tally()
// {
//     // Can we just use the base class method? (Do we need this method at all?)
//     // ...
// }

// void LinearSourceDomain::all_reduce_replicated_source_regions()
// {
//     // We will first call the base class method as it needs to reduce these
//     // variables as well
//     FlatSourceDomain::all_reduce_replicated_source_regions();
//     // Then we add in the stuff specific to the linear source class
//     // ...
// }

// void LinearSourceDomain::output_to_vtk() const
// {
//     // Can we just use the base class method? (Do we need this method at all?)
//     // ...
// }

// void LinearSourceDomain::apply_external_source_to_source_region(
//  Discrete* discrete, double strength_factor, int64_t source_region)
// {
//     Can we just use the base class method? (Do we need this method at all?)
//     ...
// }

void LinearSourceDomain::flux_swap()
{
  FlatSourceDomain::flux_swap();
  flux_x_old_.swap(flux_x_new_);
  flux_y_old_.swap(flux_y_new_);
  flux_z_old_.swap(flux_z_new_);
}


} // namespace openmc