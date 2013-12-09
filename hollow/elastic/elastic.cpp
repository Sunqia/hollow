// IGA Elasticity solver

#include <hollow/elastic/neo_hookean.h>
#include <hollow/iga/iga.h>
#include <hollow/tao/solver.h>
#include <hollow/petsc/mpi.h>
#include <geode/math/constants.h>
#include <geode/python/Class.h>
#include <geode/utility/Log.h>
namespace hollow {
namespace {

using Log::cout;
using std::endl;

// See hollow/doc/fem.tex for details.

template<class Model> struct Elastic : public IGA {
  GEODE_DECLARE_TYPE(GEODE_NO_EXPORT)
  typedef IGA Base;
  static const int d = Model::d;
  typedef PetscReal T;
  typedef Vector<T,d> TV;

  const Model model;
  const TV rho_g;

protected:
  Elastic(const MPI_Comm comm, RawArray<const T> material, const TV rho_g)
    : IGA(comm)
    , model(material)
    , rho_g(rho_g) {
    set_dim(d);
    set_dof(d);
    set_order(1); // Only need first derivatives
  }
public:

  void set_up() override {
    GEODE_ASSERT(dim()==d);
    CHECK(IGASetFormFunction(iga,residual,(void*)this));
    CHECK(IGASetFormJacobian(iga,jacobian,(void*)this));
    Base::set_up();
  }

  Ref<SNES> create_snes() const override {
    const auto snes = Base::create_snes();
    // For now, don't use energy as the objective.
    // Optimization problems should create_tao below anyways.
    if (0)
      CHECK(SNESSetObjective(snes->snes,objective<::SNES>,(void*)this));
    return snes;
  }

  Ref<TaoSolver> create_tao() const {
    const auto tao = new_<TaoSolver>(comm());
    CHECK(TaoSetType(tao->tao,"tao_nls"));
    const auto gradient = [](::TaoSolver, ::Vec u, ::Vec grad, void* ctx) {
      const Elastic& self = *(const Elastic*)ctx;
      CHECK(IGAComputeFunction(self.iga,u,grad));
      return PetscErrorCode(0);
    };
    const auto hessian = [](::TaoSolver, ::Vec u, ::Mat* A, ::Mat* P, MatStructure* flag, void* ctx) {
      const Elastic& self = *(const Elastic*)ctx;
      GEODE_ASSERT(*A==*P);
      CHECK(IGAComputeJacobian(self.iga,u,*A));
      *flag = SAME_NONZERO_PATTERN;
      PetscFunctionReturn(0);
    };
    CHECK(TaoSetObjectiveRoutine(tao->tao,objective<::TaoSolver>,(void*)this));
    CHECK(TaoSetGradientRoutine(tao->tao,gradient,(void*)this));
    const auto A = create_mat();
    CHECK(TaoSetHessianRoutine(tao->tao,A->m,A->m,hessian,(void*)this));
    return tao;
  }

  template<class Ignore> static PetscErrorCode objective(Ignore, ::Vec u, T* energy, void* ctx) {
    const Elastic& self = *(const Elastic*)ctx;
    HOLLOW_MONITOR_J(self.model.J_range = Box<T>::empty_box());
    CHECK(IGAComputeScalarCustom(self.iga,u,1,energy,Elastic::energy,ctx,PETSC_TRUE));
    if (!isfinite(*energy)) // Tao doesn't like infinite energies
      *energy = 1e10;
    HOLLOW_MONITOR_J(cout << "J_range = "<<self.model.J_range<<endl);
    return PetscErrorCode(0);
  }

  static TV phi(IGAPoint p, const T* u_) {
    TV x;
    IGAPointFormGeomMap(p,x.data());
    TV u;
    IGAPointFormValue(p,u_,u.data());
    return x+u;
  }

  static Matrix<T,d> F(IGAPoint p, const T* u_) {
    Matrix<T,d> F;
    IGAPointFormGrad(p,u_,F.data());
    return F+1;
  }

  static PetscErrorCode energy(IGAPoint p, const T* u, const int n, T* energy, void* ctx) {
    assert(n==1);
    const Elastic& self = *(const Elastic*)ctx;
    *energy = self.model.energy(F(p,u))-dot(self.rho_g,phi(p,u));
    return 0;
  }

  static PetscErrorCode residual(IGAPoint p, const T* u, T* b_, void* ctx) {
    const Elastic& self = *(const Elastic*)ctx;
    const int nen = p->nen;
    const T*  N0 = (decltype(N0))p->shape[0];
    const TV* N1 = (decltype(N1))p->shape[1];
    const auto F = self.F(p,u);
    const auto rho_g = self.rho_g;
    const auto P = self.model.stress(F);
    TV* b = (TV*)b_;
    for (int a=0;a<nen;a++)
      b[a] = P*N1[a] - rho_g*N0[a]; // elasticity + gravity
    return 0;
  }

  static PetscErrorCode jacobian(IGAPoint p, const T* u, T* A_, void* ctx) {
    const Elastic& self = *(const Elastic*)ctx;
    const int nen = p->nen;
    const T*  N0 = (decltype(N0))p->shape[0];
    const TV* N1 = (decltype(N1))p->shape[1];
    const auto F = self.F(p,u);
    TV* A = (decltype(A))A_;
    for (int a=0;a<nen;a++)
      for (int i=0;i<d;i++) {
        Matrix<T,d> dF;
        dF[i] = N1[a];
        const auto dP = self.model.differential(F,dF);
        for (int b=0;b<nen;b++)
          A[(a*d+i)*nen+b] = dP*N1[b];
      }
    return 0;
  }
};

template<> GEODE_DEFINE_TYPE(Elastic<NeoHookean<2>>)
template<> GEODE_DEFINE_TYPE(Elastic<NeoHookean<3>>)

}
}
using namespace hollow;

template<class Model> static void wrap_helper(const char* name) {
  typedef Elastic<Model> Self;
  typedef typename Self::T T;
  typedef typename Self::TV TV;
  Class<Self>(name)
    .GEODE_INIT(MPI_Comm,RawArray<const T>,TV)
    .GEODE_METHOD(create_tao)
    ;
}

void wrap_elastic() {
  wrap_helper<NeoHookean<2>>("NeoHookeanElastic2d");
  wrap_helper<NeoHookean<3>>("NeoHookeanElastic3d");
}
