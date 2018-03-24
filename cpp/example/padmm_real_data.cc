#include <array>
#include <memory>
#include <random>
#include <boost/math/special_functions/erf.hpp>
#include <sopt/credible_region.h>
#include <sopt/imaging_padmm.h>
#include <sopt/relative_variation.h>
#include <sopt/utilities.h>
#include <sopt/wavelets.h>
#include <sopt/wavelets/sara.h>
#include "purify/directories.h"
#include "purify/logging.h"
#include "purify/operators.h"
#ifdef PURIFY_GPU
#include "purify/operators_gpu.h"
#endif
#include "purify/cimg.h"
#include "purify/pfitsio.h"
#include "purify/types.h"
#include "purify/utilities.h"
#include "purify/uvfits.h"
#include "purify/wproj_utilities.h"
using namespace purify;
using namespace purify::notinstalled;

void padmm(const std::string &name, const t_uint &imsizex, const t_uint &imsizey,
           const kernels::kernel kernel, const t_int J, const utilities::vis_params &uv_data,
           const t_real sigma, const std::tuple<bool, t_real> &w_term) {

  std::string const outfile_fits = output_filename(name + "_solution.fits");
  std::string const residual_fits = output_filename(name + "_residual.fits");
  std::string const dirty_image_fits = output_filename(name + "_dirty.fits");
  std::string const psf_image_fits = output_filename(name + "_psf.fits");

  t_real const over_sample = 2;
  std::shared_ptr<sopt::LinearTransform<Vector<t_complex>> const> measurements_transform
      = measurementoperator::init_degrid_operator_2d<Vector<t_complex>>(
          uv_data, imsizey, imsizex, std::get<1>(w_term), std::get<1>(w_term), over_sample, 100,
          0.0001, kernel, J, J, operators::fftw_plan::measure, std::get<0>(w_term), 12);
  t_uint const M = uv_data.size();
  t_uint const N = imsizex * imsizey;
  sopt::wavelets::SARA const sara{
      std::make_tuple("Dirac", 3u), std::make_tuple("DB1", 3u), std::make_tuple("DB2", 3u),
      std::make_tuple("DB3", 3u),   std::make_tuple("DB4", 3u), std::make_tuple("DB5", 3u),
      std::make_tuple("DB6", 3u),   std::make_tuple("DB7", 3u), std::make_tuple("DB8", 3u)};

  auto const Psi = sopt::linear_transform<t_complex>(sara, imsizey, imsizex);
  const Vector<> dimage = (measurements_transform->adjoint() * uv_data.vis).real();
  Matrix<t_complex> point = Matrix<t_complex>::Zero(imsizey, imsizex);
  point(std::floor(imsizey / 2), std::floor(imsizex / 2)) = 1.;
  const Vector<> psf
      = (measurements_transform->adjoint()
         * (*measurements_transform * Vector<t_complex>::Map(point.data(), point.size())))
            .real();
  Vector<t_complex> initial_estimate = Vector<t_complex>::Zero(dimage.size());
  pfitsio::write2d(Image<t_real>::Map(dimage.data(), imsizey, imsizex), dirty_image_fits);
  pfitsio::write2d(Image<t_real>::Map(psf.data(), imsizey, imsizex), psf_image_fits);
  return;
  auto const epsilon = utilities::calculate_l2_radius(uv_data.vis, sigma);
  PURIFY_HIGH_LOG("Using epsilon of {}", epsilon);
#ifdef PURIFY_CImg
  auto const canvas
      = std::make_shared<CDisplay>(cimg::make_display(Vector<t_real>::Zero(1024 * 512), 1024, 512));
  canvas->resize(true);
  auto const show_image = [&, measurements_transform](const Vector<t_complex> &x) -> bool {
    if(!canvas->is_closed()) {
      const Vector<t_complex> res
          = (measurements_transform->adjoint() * (uv_data.vis - (*measurements_transform * x)));
      const auto img1 = cimg::make_image(x.real().eval(), imsizey, imsizex)
                            .get_normalize(0, 1)
                            .get_resize(512, 512);
      const auto img2 = cimg::make_image(res.real().eval(), imsizey, imsizex)
                            .get_normalize(0, 1)
                            .get_resize(512, 512);
      const auto results = CImageList<t_real>(img1, img2);
      canvas->display(results);
      canvas->resize(true);
    }
    return true;
  };
#endif
  auto const padmm
      = sopt::algorithm::ImagingProximalADMM<t_complex>(uv_data.vis)
            .itermax(500)
            .gamma((measurements_transform->adjoint() * uv_data.vis).real().maxCoeff() * 1e-3)
            .relative_variation(1e-3)
            .l2ball_proximal_epsilon(epsilon)
            .tight_frame(false)
            .l1_proximal_tolerance(1e-2)
            .l1_proximal_nu(1.)
            .l1_proximal_itermax(50)
            .l1_proximal_positivity_constraint(true)
            .l1_proximal_real_constraint(true)
            .residual_convergence(epsilon * 1.001)
            .lagrange_update_scale(0.9)
#ifdef PURIFY_CImg
            .is_converged(show_image)
#endif
            .nu(1e0)
            .Psi(Psi)
            .Phi(*measurements_transform);

  auto const diagnostic = padmm();
  Image<t_complex> image = Image<t_complex>::Map(diagnostic.x.data(), imsizey, imsizex);
  pfitsio::write2d(image.real(), outfile_fits);
  Vector<t_complex> residuals = measurements_transform->adjoint()
                                * (uv_data.vis - ((*measurements_transform) * diagnostic.x));
  Image<t_complex> residual_image = Image<t_complex>::Map(residuals.data(), imsizey, imsizex);
  pfitsio::write2d(residual_image.real(), residual_fits);
#ifdef PURIFY_CImg
  const auto results = CImageList<t_real>(
      cimg::make_image(diagnostic.x.real().eval(), imsizey, imsizex).get_resize(512, 512),
      cimg::make_image(residuals.real().eval(), imsizey, imsizex).get_resize(512, 512));
  canvas->display(results);
  cimg::make_image(residuals.real().eval(), imsizey, imsizex)
      .histogram(256)
      .display_graph("Residual Histogram", 2);
  while(!canvas->is_closed())
    canvas->wait();
#endif
}

int main(int, char **) {
  sopt::logging::initialize();
  purify::logging::initialize();
  sopt::logging::set_level("debug");
  purify::logging::set_level("debug");
  const std::string &name = "real_data";
  const bool w_term = true;
  const t_real cellsize = 20;
  const t_uint imsizex = 512;
  const t_uint imsizey = 512;
  std::string const inputfile = vla_filename("../mwa/uvdump_01.vis");

  auto uv_data = utilities::read_visibility(inputfile, true);
  uv_data = utilities::sort_by_w(uv_data);
  t_real const sigma = uv_data.weights.norm() / std::sqrt(uv_data.weights.size());
  uv_data.vis = uv_data.vis.array() * uv_data.weights.array();
  std::cout << uv_data.u.array().mean() << " " << uv_data.v.array().mean() << " "
            << uv_data.w.array().mean() << std::endl;
  padmm(name, imsizex, imsizey, kernels::kernel::kb, 4, uv_data, sigma,
        std::make_tuple(w_term, cellsize));
  return 0;
}
