
#include <stdexcept>
#include "RigidBodySystem.h"
#include "RigidBodyIK.h"  // required for resolving initial conditions
#include "urdfParsingUtil.h"

using namespace std;
using namespace Eigen;
using namespace Drake;


RigidBodySystem::StateVector<double> RigidBodySystem::dynamics(const double& t, const RigidBodySystem::StateVector<double>& x, const RigidBodySystem::InputVector<double>& u) const {
  using namespace std;
  using namespace Eigen;
  eigen_aligned_unordered_map<const RigidBody *, Matrix<double, 6, 1> > f_ext;

  // todo: make kinematics cache once and re-use it (but have to make one per type)
  auto nq = tree->num_positions;
  auto nv = tree->num_velocities;
  auto num_actuators = tree->actuators.size();
  auto q = x.topRows(nq);
  auto v = x.bottomRows(nv);
  auto kinsol = tree->doKinematics(q,v);

  auto H = tree->massMatrix(kinsol);

  const NullVector<double> force_state;  // todo:  will have to handle this case

  { // loop through rigid body force elements and accumulate f_ext
    int u_index = 0;
    for (auto const & prop : props) {
      RigidBodyFrame* frame = prop->getFrame();
      RigidBody* body = frame->body.get();
      int num_inputs = 1;  // todo: generalize this
      RigidBodyPropellor::InputVector<double> u_i(u.middleRows(u_index,num_inputs));
      // todo: push the frame to body transform into the dynamicsBias method?
      Matrix<double,6,1> f_ext_i = transformSpatialForce(frame->transform_to_body,prop->output(t,force_state,u_i,kinsol));
      if (f_ext.find(body)==f_ext.end()) f_ext[body] = f_ext_i;
      else f_ext[body] = f_ext[body]+f_ext_i;
      u_index += num_inputs;
    }
  }

  VectorXd C = tree->dynamicsBiasTerm(kinsol,f_ext);
  if (num_actuators > 0) C -= tree->B*u.topRows(num_actuators);

  { // apply contact forces
    VectorXd phi;
    Matrix3Xd normal, xA, xB;
    vector<int> bodyA_idx, bodyB_idx;
    tree->collisionDetect(kinsol,phi,normal,xA,xB,bodyA_idx,bodyB_idx);
//    tree->potentialCollisions(kinsol,phi,normal,xA,xB,bodyA_idx,bodyB_idx);
    std::cout << "phi = " << phi.transpose() << std::endl;

    for (int i=0; i<phi.rows(); i++) {
      if (phi(i)<0.0) { // then i have contact
        // spring law for normal force:  fA = (-k*phi - b*phidot)*normal
        double k = 500, b = k/10;  // todo: put these somewhere better... or make them parameters?
        auto JA = tree->forwardKinJacobian(kinsol,xA.col(i),bodyA_idx[i],0,0,false);
        auto JB = tree->forwardKinJacobian(kinsol,xB.col(i),bodyB_idx[i],0,0,false);
        auto relative_velocity = (JA-JB)*v;
        auto phidot = relative_velocity.dot(normal.col(i));
        auto fA_normal = -k*phi(i) - b*phidot;

        // Coulomb sliding friction (no static friction yet, because it is a complementarity problem):
        auto tangential_velocity = relative_velocity-phidot*normal.col(i);
        double mu = 1.0; // todo: make this a parameter
        auto fA = fA_normal*normal.col(i)- mu*fA_normal*tangential_velocity/(tangential_velocity.norm() + 1e-12);  // 1e-12 to avoid divide by zero

        // equal and opposite: fB = -fA.
        // tau = JA^T fA + JB^fB
        C -= JA.transpose()*fA - JB.transpose()*fA;
      }
    }
  }

  // todo: lots of code performance optimization possible here... I'm doing an exorbitant amount of identical allocations on every function evaluation.  Just keeping it simple at first.
  OptimizationProblem prog;
  auto const & vdot = prog.addContinuousVariables(nv,"vdot");

  Eigen::MatrixXd H_and_neg_JT = H;
  if (tree->getNumPositionConstraints()) {
    int nc = tree->getNumPositionConstraints();
    const double alpha = 5.0;  // 1/time constant of position constraint satisfaction (see my latex rigid body notes)

    prog.addContinuousVariables(nc,"position constraint force");  // don't actually need to use the decision variable reference that would be returned...

    // then compute the constraint force
    auto phi = tree->positionConstraints(kinsol);
    auto J = tree->positionConstraintsJacobian(kinsol,false);
    auto Jdotv = tree->positionConstraintsJacDotTimesV(kinsol);

    // phiddot = -2 alpha phidot - alpha^2 phi  (0 + critically damped stabilization term)
    prog.addLinearEqualityConstraint(J,-(Jdotv + 2*alpha*J*v + alpha*alpha*phi),vdot);
    H_and_neg_JT.conservativeResize(NoChange,H_and_neg_JT.cols()+J.rows());
    H_and_neg_JT.rightCols(J.rows()) = -J.transpose();
  }

  // add [H,-J^T]*[vdot;f] = -C
  prog.addLinearEqualityConstraint(H_and_neg_JT,-C);

  prog.solve();
  //      prog.printSolution();

  StateVector<double> dot(nq+nv);
  dot << kinsol.transformPositionDotMappingToVelocityMapping(Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic>::Identity(nq, nq))*v, vdot.value;
  return dot;
}


RigidBodySystem::StateVector<double> Drake::getInitialState(const RigidBodySystem& sys) {

  VectorXd x0 = Matrix<double,Dynamic,1>::Random(sys.tree->num_positions+sys.tree->num_velocities);

  if (sys.tree->getNumPositionConstraints()) {
    // todo: move this up to the system level?

    std::vector<RigidBodyLoop,Eigen::aligned_allocator<RigidBodyLoop> > const& loops = sys.tree->loops;

    int nq = sys.tree->num_positions;
    int num_constraints = 2*loops.size();
    RigidBodyConstraint** constraint_array = new RigidBodyConstraint*[num_constraints];

    Matrix<double,7,1> bTbp = Matrix<double,7,1>::Zero();  bTbp(3)=1.0;
    Vector2d tspan; tspan<<-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity();
    Vector3d zero = Vector3d::Zero();
    for (int i=0; i<loops.size(); i++) {
      constraint_array[2*i] = new RelativePositionConstraint(sys.tree.get(), zero, zero, zero, loops[i].frameA->frame_index, loops[i].frameB->frame_index,bTbp,tspan);
      constraint_array[2*i+1] = new RelativePositionConstraint(sys.tree.get(), loops[i].axis, loops[i].axis, loops[i].axis, loops[i].frameA->frame_index, loops[i].frameB->frame_index,bTbp,tspan);
    }

    int info;
    vector<string> infeasible_constraint;
    IKoptions ikoptions(sys.tree.get());

    VectorXd q_guess = x0.topRows(nq);
    VectorXd q(nq);

    inverseKin(sys.tree.get(),q_guess,q_guess,num_constraints,constraint_array,q,info,infeasible_constraint,ikoptions);
    if (info>=10) {
      cout << "INFO = " << info << endl;
      cout << infeasible_constraint.size() << " infeasible constraints:";
      size_t limit = infeasible_constraint.size();
      if (limit>5) { cout << " (only printing first 5)" << endl; limit=5; }
      cout << endl;
      for (int i=0; i<limit; i++)
        cout << infeasible_constraint[i] << endl;
    }
    x0 << q, VectorXd::Zero(sys.tree->num_velocities);

    for (int i=0; i<num_constraints; i++) {
      delete constraint_array[i];
    }
    delete[] constraint_array;
  }
  return x0;
}

RigidBodyPropellor::RigidBodyPropellor(RigidBodySystem *sys, TiXmlElement *node, std::string name) :
        sys(sys), name(name),
        scale_factor_thrust(1.0), scale_factor_moment(1.0),
        lower_limit(-numeric_limits<double>::infinity()),
        upper_limit(numeric_limits<double>::infinity())
{
  auto tree = sys->getRigidBodyTree();

  TiXmlElement* parent_node = node->FirstChildElement("parent");
  if (!parent_node) throw runtime_error("propellor " + name + " is missing the parent node");
  frame = allocate_shared<RigidBodyFrame>(aligned_allocator<RigidBodyFrame>(),tree.get(),parent_node,node->FirstChildElement("origin"),name+"Frame");
  tree->addFrame(frame);

  axis << 1.0, 0.0, 0.0;
  TiXmlElement* axis_node = node->FirstChildElement("axis");
  if (axis_node) {
    parseVectorAttribute(axis_node, "xyz", axis);
    if (axis.norm()<1e-8) throw runtime_error("ERROR: axis is zero.  don't do that");
    axis.normalize();
  }

  parseScalarAttribute(node,"scale_factor_thrust",scale_factor_thrust);
  parseScalarAttribute(node,"scale_factor_moment",scale_factor_moment);
  parseScalarAttribute(node,"lower_limit",lower_limit);
  parseScalarAttribute(node,"upper_limit",upper_limit);

  // todo: parse visual info?
}

void parseForceElement(RigidBodySystem *sys, TiXmlElement* node) {
  string name = node->Attribute("name");

  if (TiXmlElement* propellor_node = node->FirstChildElement("propellor")) {
    sys->addForceElement(allocate_shared<RigidBodyPropellor>(Eigen::aligned_allocator<RigidBodyPropellor>(),sys,propellor_node,name));
  }
}

void parseRobot(RigidBodySystem *sys, TiXmlElement* node)
{
  if (!node->Attribute("name"))
    throw runtime_error("Error: your robot must have a name attribute");
  string robotname = node->Attribute("name");

  // parse force elements
  for (TiXmlElement* force_node = node->FirstChildElement("force_element"); force_node; force_node = force_node->NextSiblingElement("force_element"))
    parseForceElement(sys, force_node);
}

void parseURDF(RigidBodySystem *sys, TiXmlDocument *xml_doc)
{
  TiXmlElement *node = xml_doc->FirstChildElement("robot");
  if (!node) throw std::runtime_error("ERROR: This urdf does not contain a robot tag");

  parseRobot(sys, node);
}

void RigidBodySystem::addRobotFromURDFString(const string &xml_string, const string &root_dir, const DrakeJoint::FloatingBaseType floating_base_type)
{
  // first add the urdf to the rigid body tree
  tree->addRobotFromURDFString(xml_string, root_dir, floating_base_type);

  // now parse additional tags understood by rigid body system (actuators, sensors, etc)
  TiXmlDocument xml_doc;
  xml_doc.Parse(xml_string.c_str());
  parseURDF(this,&xml_doc);
}


void RigidBodySystem::addRobotFromURDF(const string &urdf_filename, const DrakeJoint::FloatingBaseType floating_base_type)
{
  // first add the urdf to the rigid body tree
  tree->addRobotFromURDF(urdf_filename, floating_base_type);

  // now parse additional tags understood by rigid body system (actuators, sensors, etc)
  TiXmlDocument xml_doc(urdf_filename);
  if (!xml_doc.LoadFile()) {
    throw std::runtime_error("failed to parse xml in file " + urdf_filename + "\n" + xml_doc.ErrorDesc());
  }
  parseURDF(this,&xml_doc);
}

