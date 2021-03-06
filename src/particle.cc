#include "particle.h"

#include <algorithm>
#include <cassert>
#include <iostream>

namespace SPHack {

ParticleSystem::ParticleSystem(const AABB& bounds, Real radius) : bounds_(bounds), radius_(radius), kernel_(radius) {
  available_particles_.reserve(kMaxParticles);
  for (int i = kMaxParticles - 1; i >= 0; --i) {
    available_particles_.push_back(i);
  }

  gravity_ = Vec2(0.0, -1.0);

  grid_delta_ = radius;
  grid_width_ = static_cast<int>(ceil(bounds.width() / grid_delta_));
  grid_height_ = static_cast<int>(ceil(bounds.height() / grid_delta_));
  grid_.resize(grid_width_*grid_height_);
  static const int estimated_particles_per_cell = 16;
  for (auto& grid_cell : grid_) {
    grid_cell.reserve(estimated_particles_per_cell);
  }

  radius2_ = radius_ * radius_;
  static const Real density = 1.7e2 / radius2_;
  density_inv_ = 1.0 / density;
  cfm_epsilon_ = 1e3;

  kernel_ = KernelEvaluator(radius_);
  
  boundary_margin_ = radius_ * 0.5;

  random_.seed(144438);
  jitter_dist_ = std::uniform_real_distribution<>(-1e-6, 1e-6);
}

void ParticleSystem::AddParticles(const AABB& region, int max) {
  AABB clipped_region = bounds_.Intersect(region);

  int particles_added = 0;
  for (Real x = clipped_region.min()[0] + boundary_margin_; x < clipped_region.max()[0] - boundary_margin_; x += 0.6*radius_) {
    for (Real y = clipped_region.min()[1] + boundary_margin_; y < clipped_region.max()[1] - boundary_margin_; y += 0.6*radius_) {
      if (max >= 0 && particles_added >= max) {
        continue;
      }
      if (!CreateParticle(Vec2(x, y), Vec2(0.0, 0.0))) {
        std::cerr << "Failed to create particle at: " << x << " " << y << std::endl;
      } else {
        ++particles_added;
      }
    }
  }
  std::cerr << particles_added << " particles added" << std::endl;
}

void ParticleSystem::ApplyForces(Real dt) {
  for (size_t i = 0; i < size(); ++i) {
    vel_[i] += gravity_ * dt;
  }
}

void ParticleSystem::PredictPositions(Real dt) {
  for (size_t i = 0; i < size(); ++i) {
    predicted_pos_[i] = dt * vel_[i] + pos_[i];
  }
}

void ParticleSystem::UpdateVelocities(Real dt) {
  for (size_t i = 0; i < size(); ++i) {
    Vec2 old_vel = vel_[i];
    vel_[i] = (predicted_pos_[i] - pos_[i]) * (1.0 / dt);
    accel_[i] += (vel_[i] - old_vel) / dt;
  }
}

void ParticleSystem::CommitPositions() {
  pos_.swap(predicted_pos_);
}

void ParticleSystem::Step(Real dt) {
  static const int kSubsteps = 2;
  dt /= kSubsteps;

  for (size_t i = 0; i < size(); ++i) {
    accel_[i] = Vec2(0.0, 0.0);
  }

  for (int substep = 0; substep < kSubsteps; ++substep) {
    ApplyForces(dt);
    PredictPositions(dt);
    BuildGrid();
    static const int kIters = 4;
    for (int i = 0; i < kIters; ++i) {
      SubstepResetGrid();
      CalculateLambdaOnGrid();
      CalculatePressureDeltaOnGrid();
      ApplyPressureDeltaOnGrid();
      EnforceBoundariesOnGrid();
    }
    CopyPositionsFromGrid();
    UpdateVelocities(dt);
    ApplyViscosityOnGrid(dt);
    CommitPositions();
  }
}

void ParticleSystem::InitDensity() {
  for (size_t i = 0; i < pos_.size(); i++) {
    predicted_pos_[i] = pos_[i];
  }
  BuildGrid();
  CalculateLambdaOnGrid();
  Real max_density = 0.0;
  for (size_t i = 0; i < density_.size(); i++) {
    if (isActive(i)) {
      max_density = std::max(max_density, density_[i]);
    }
  }

  density_inv_ /= max_density;
  cfm_epsilon_ = 0.03 / density_inv_;
  std::cerr << "density: " << (1.0 / density_inv_) << std::endl;
  std::cerr << "cfm eps: " << cfm_epsilon_ << std::endl;
}

void ParticleSystem::Clear() {
  available_particles_.clear();
  for (int i = kMaxParticles - 1; i >= 0; --i) {
    available_particles_.push_back(i);
    flag_[i] = 0;
  }
}

bool ParticleSystem::CreateParticle(const Vec2& pos, const Vec2& vel) {
  if (available_particles_.empty()) {
    return false;
  }

  ParticleIDType id = available_particles_.back();
  available_particles_.pop_back();
  pos_[id] = pos;
  vel_[id] = vel;
  accel_[id] = Vec2();
  flag_[id] = PARTICLE_FLAG_ACTIVE;

  return true;
}

void ParticleSystem::BuildGrid() {
  for (auto& grid_cell : grid_) {
    grid_cell.clear();
  }

  for (size_t i = 0; i < predicted_pos_.size(); ++i) {
    if (isActive(i)) {
      int x = static_cast<int>(predicted_pos_[i].x / grid_delta_);
      x = std::max(0, std::min(grid_width_ - 1, x));
      int y = static_cast<int>(predicted_pos_[i].y / grid_delta_);
      y = std::max(0, std::min(grid_height_ - 1, y));

      grid_[CellID(x, y)].push_back(
          PressureParticle {
              predicted_pos_[i],
              Vec2(0.0, 0.0),
              static_cast<ParticleIDType>(i)
          });
    }
  }
}

void ParticleSystem::SubstepResetGrid() {
  for (int ox = 0; ox < grid_width_; ++ox) {
    for (int oy = 0; oy < grid_height_; ++oy) {
      int cell_i = CellID(ox, oy);
      for (int i = 0; i < grid_[cell_i].size(); ++i) {
        PressureParticle& pi = grid_[cell_i][i];
        pi.lambda = 0.0;
        pi.density = 0.0;
        pi.cj_grad_norm_squared_sum = 0.0;
        pi.ci_grad_sum = Vec2(0.0, 0.0);
        pi.pos_delta = Vec2(0.0, 0.0);
      }
    }
  }
}

void ParticleSystem::AccumulateLambdaData(PressureParticle& pi, PressureParticle& pj) {
  const Vec2 r = pi.pos - pj.pos;
  if (r.squaredNorm() > radius2_) return;
  const Real poly6_weight = kernel_.Poly6NonNorm(r);
  pi.density += poly6_weight;
  pj.density += poly6_weight;
  const Vec2 grad = kernel_.SpikyGradNonNorm(r);
  const Real gradSquaredNorm = grad.squaredNorm();
  pi.cj_grad_norm_squared_sum += gradSquaredNorm;
  pj.cj_grad_norm_squared_sum += gradSquaredNorm;
  pi.ci_grad_sum += grad;
  pj.ci_grad_sum -= grad;
}

void ParticleSystem::CalculateLambdaOnGrid() {
  for (int ox = 0; ox < grid_width_; ++ox) {
    for (int oy = 0; oy < grid_height_; ++oy) {
      int cell_i = CellID(ox, oy);
      for (int i = 0; i < grid_[cell_i].size(); ++i) {
        PressureParticle& pi = grid_[cell_i][i];

        for (int j = i+1; j < grid_[cell_i].size(); ++j) {
          PressureParticle& pj = grid_[cell_i][j];
          AccumulateLambdaData(pi, pj);
        }
      }

      for (int dx = 0; dx <= 1; ++dx) {
        const int nx = ox + dx;
        if (nx >= grid_width_) continue;
        for (int dy = ((dx == 0) ? 1 : -1); dy <= 1; ++dy) {
          const int ny = oy + dy;
          if (ny < 0) continue;
          if (ny >= grid_width_) continue;

          int cell_j = CellID(nx, ny);

          for (int i = 0; i < grid_[cell_i].size(); ++i) {
            PressureParticle& pi = grid_[cell_i][i];
            for (int j = 0; j < grid_[cell_j].size(); ++j) {
              PressureParticle& pj = grid_[cell_j][j];
              AccumulateLambdaData(pi, pj);
            }
          }
        }
      }

      for (int i = 0; i < grid_[cell_i].size(); ++i) {
        PressureParticle& pi = grid_[cell_i][i];

        pi.density *= density_inv_ * kernel_.poly6_norm();
        density_[pi.id] = pi.density;
        const Real den = (pi.cj_grad_norm_squared_sum*kernel_.spiky_grad_norm()*kernel_.spiky_grad_norm() + (pi.ci_grad_sum * kernel_.spiky_grad_norm()).squaredNorm())*density_inv_*density_inv_ + cfm_epsilon_;
        pi.lambda = -(pi.density - 1.0) / den;
      }
    }
  }
}

void ParticleSystem::AccumulatePressureDelta(PressureParticle& pi, PressureParticle& pj) {
  const Vec2 r = pi.pos - pj.pos;
  if (r.squaredNorm() > radius2_) return;
  Real scorr = kernel_.Poly6NonNorm(r)/kernel_.Poly6NonNorm(Vec2(0.0,0.2*radius_));
  scorr = scorr*scorr*scorr*scorr;
  scorr *= -0.000005;
  const Vec2 delta = (pi.lambda + pj.lambda + scorr) * kernel_.SpikyGradNonNorm(r);
  pi.pos_delta += delta;
  pj.pos_delta -= delta;
}

void ParticleSystem::CalculatePressureDeltaOnGrid() {
  for (int ox = 0; ox < grid_width_; ++ox) {
    for (int oy = 0; oy < grid_height_; ++oy) {
      int cell_i = CellID(ox, oy);
      for (int i = 0; i < grid_[cell_i].size(); ++i) {
        PressureParticle& pi = grid_[cell_i][i];

        for (int j = i+1; j < grid_[cell_i].size(); ++j) {
          PressureParticle& pj = grid_[cell_i][j];
          AccumulatePressureDelta(pi, pj);
        }
      }

      for (int dx = 0; dx <= 1; ++dx) {
        const int nx = ox + dx;
        if (nx >= grid_width_) continue;
        for (int dy = ((dx == 0) ? 1 : -1); dy <= 1; ++dy) {
          const int ny = oy + dy;
          if (ny < 0) continue;
          if (ny >= grid_width_) continue;

          int cell_j = CellID(nx, ny);

          for (int i = 0; i < grid_[cell_i].size(); ++i) {
            PressureParticle& pi = grid_[cell_i][i];
            for (int j = 0; j < grid_[cell_j].size(); ++j) {
              PressureParticle& pj = grid_[cell_j][j];
              AccumulatePressureDelta(pi, pj);
            }
          }
        }
      }

      for (int i = 0; i < grid_[cell_i].size(); ++i) {
        PressureParticle& pi = grid_[cell_i][i];
        pi.pos_delta *= density_inv_ * kernel_.spiky_grad_norm();
      }
    }
  }
}

void ParticleSystem::ApplyPressureDeltaOnGrid() {
  for (int ox = 0; ox < grid_width_; ++ox) {
    for (int oy = 0; oy < grid_height_; ++oy) {
      for (auto& pi : grid_[CellID(ox, oy)]) {
        pi.pos += pi.pos_delta;
      }
    }
  }
}

void ParticleSystem::EnforceBoundariesOnGrid() {
  AABB shrunk_bounds = bounds_.Shrink(boundary_margin_);
  for (auto& grid_cell : grid_) {
    for (auto& particle : grid_cell) {
      if (!shrunk_bounds.IsInside(particle.pos)) {
        particle.pos = shrunk_bounds.Clip(particle.pos);
        particle.pos.x += jitter_dist_(random_);
        particle.pos.y += jitter_dist_(random_);
      }
    }
  }
}

void ParticleSystem::CopyPositionsFromGrid() {
  for (auto& grid_cell : grid_) {
    for (auto& particle: grid_cell) {
      predicted_pos_[particle.id] = particle.pos;
    }
  }
}

void ParticleSystem::ApplyViscosityOnGrid(Real dt) {
   
}
  
}  // namespace SPHack
