#pragma once

#include <Eigen/Dense>
#include <Eigen/StdVector>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include "drake/drakeRBM_export.h"
#include "drake/systems/plants/collision/DrakeCollision.h"
#include "drake/systems/plants/joints/DrakeJoint.h"

class DRAKERBM_EXPORT RigidBody {
 private:
  std::unique_ptr<DrakeJoint> joint;

  // TODO(amcastro-tri): move this to CollisionElement.
  DrakeCollision::bitmask collision_filter_group;
  DrakeCollision::bitmask collision_filter_ignores;

 public:
  RigidBody();

  void setJoint(std::unique_ptr<DrakeJoint> joint);
  const DrakeJoint& getJoint() const;

  bool hasParent() const;

  /**
   * Checks if a particular rigid body is the parent of this rigid body.
   *
   * @param[in] other The potential parent of this rigid body.
   * @return true if the supplied rigid body parameter other is the parent of
   * this rigid body.
   */
  bool has_as_parent(const RigidBody& other) const { return parent == &other; }

  void addVisualElement(const DrakeShapes::VisualElement& elements);

  const DrakeShapes::VectorOfVisualElements& getVisualElements() const;

  void setCollisionFilter(const DrakeCollision::bitmask& group,
                          const DrakeCollision::bitmask& ignores);

  const DrakeCollision::bitmask& getCollisionFilterGroup() const {
    return collision_filter_group;
  }
  void setCollisionFilterGroup(const DrakeCollision::bitmask& group) {
    this->collision_filter_group = group;
  }

  const DrakeCollision::bitmask& getCollisionFilterIgnores() const {
    return collision_filter_ignores;
  }
  void setCollisionFilterIgnores(const DrakeCollision::bitmask& ignores) {
    this->collision_filter_ignores = ignores;
  }

  void addToCollisionFilterGroup(const DrakeCollision::bitmask& group) {
    this->collision_filter_group |= group;
  }
  void ignoreCollisionFilterGroup(const DrakeCollision::bitmask& group) {
    this->collision_filter_ignores |= group;
  }
  void collideWithCollisionFilterGroup(const DrakeCollision::bitmask& group) {
    this->collision_filter_ignores &= ~group;
  }

  // TODO(amcastro-tri): Change to is_adjacent_to().
  bool adjacentTo(const RigidBody& other) const {
    return ((has_as_parent(other) && !(joint && joint->isFloating())) ||
            (other.has_as_parent(*this) &&
             !(other.joint && other.joint->isFloating())));
  }

  bool CollidesWith(const RigidBody& other) const {
    bool ignored =
        this == &other || adjacentTo(other) ||
        (collision_filter_group & other.getCollisionFilterIgnores()).any() ||
        (other.getCollisionFilterGroup() & collision_filter_ignores).any();
    return !ignored;
  }

  bool appendCollisionElementIdsFromThisBody(
      const std::string& group_name,
      std::vector<DrakeCollision::ElementId>& ids) const;

  bool appendCollisionElementIdsFromThisBody(
      std::vector<DrakeCollision::ElementId>& ids) const;

  /**
   * Transforms all of the visual, collision, and inertial elements associated
   * with this body to the proper joint frame.  This is necessary, for instance,
   * to support SDF loading where the child frame can be specified independently
   * from the joint frame. In our RigidBodyTree classes, the body frame IS the
   * joint frame.
   *
   * @param transform_body_to_joint The transform from this body's frame to the
   * joint's frame.
   */
  void ApplyTransformToJointFrame(
      const Eigen::Isometry3d& transform_body_to_joint);

 public:
  std::string linkname;
  std::string model_name;  // todo: replace robotnum w/ model_name
  int robotnum;            // uses 0-index. starts from 0
  // note: it's very ugly, but parent, dofnum, and pitch also exist currently
  // (independently) at the RigidBodyTree level to represent the featherstone
  // structure.  this version is for the kinematics.

  // TODO(amcastro-tri): Make it private and change to parent_.
  RigidBody* parent;
  int body_index;
  int position_num_start;
  int velocity_num_start;

  DrakeShapes::VectorOfVisualElements visual_elements;

  std::vector<DrakeCollision::ElementId> collision_element_ids;

  typedef std::vector<DrakeCollision::CollisionElement*>
      CollisionElementsVector;
  typedef
      typename CollisionElementsVector::iterator collision_elements_iterator;
  typedef typename CollisionElementsVector::const_iterator
      const_collision_elements_iterator;
  std::map<std::string, std::vector<DrakeCollision::ElementId> >
      collision_element_groups;

  collision_elements_iterator collision_elements_begin() {
    return collision_elements_.begin();
  }

  collision_elements_iterator collision_elements_end() {
    return collision_elements_.end();
  }

  void add_collision_element(DrakeCollision::CollisionElement* e) {
    collision_elements_.push_back(e);
  }

  /**
   * @brief Adds body to a given collision group by group id.
   *
   * This call adds each of the collision elements in this body to the provided
   * collision group.
   *
   * @param[in] group_id Collision group id. Collision elements in this group
   * do not interact.
   */
  void add_to_collision_group(int group_id);

  Eigen::Matrix3Xd contact_pts;

  double mass;
  Eigen::Vector3d com;
  Eigen::Matrix<double, TWIST_SIZE, TWIST_SIZE> I;

  friend std::ostream& operator<<(std::ostream& out, const RigidBody& b);

 public:
#ifndef SWIG
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
#endif

 private:
  CollisionElementsVector collision_elements_;
};
