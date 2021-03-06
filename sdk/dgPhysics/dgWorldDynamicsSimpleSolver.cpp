/* Copyright (c) <2003-2016> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"
#include "dgBody.h"
#include "dgWorld.h"
#include "dgConstraint.h"
#include "dgDynamicBody.h"
#include "dgDynamicBody.h"
#include "dgSkeletonContainer.h"
#include "dgCollisionInstance.h"
#include "dgWorldDynamicUpdate.h"
#include "dgBilateralConstraint.h"


void dgWorldDynamicUpdate::ResolveClusterForces(dgBodyCluster* const cluster, dgInt32 threadID, dgFloat32 timestep) const
{
	if (cluster->m_activeJointCount) {
		SortClusters(cluster, timestep, threadID);
	}

	if (!cluster->m_isContinueCollision) {
		if (cluster->m_activeJointCount) {
			BuildJacobianMatrix (cluster, threadID, timestep);
			CalculateClusterReactionForces(cluster, threadID, timestep, DG_SOLVER_MAX_ERROR);
		} else {
			IntegrateExternalForce(cluster, timestep, threadID);
		}
		IntegrateVelocity (cluster, DG_SOLVER_MAX_ERROR, timestep, threadID); 
	} else {
		// calculate reaction forces and new velocities
		BuildJacobianMatrix (cluster, threadID, timestep);
		IntegrateReactionsForces (cluster, threadID, timestep, DG_SOLVER_MAX_ERROR);

		// see if the island goes to sleep
		bool isAutoSleep = true;
		bool stackSleeping = true;
		dgInt32 sleepCounter = 10000;

		dgWorld* const world = (dgWorld*) this;
		const dgInt32 bodyCount = cluster->m_bodyCount;
		dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
		dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart];

		const dgFloat32 forceDamp = DG_FREEZZING_VELOCITY_DRAG;
		dgFloat32 maxAccel = dgFloat32 (0.0f);
		dgFloat32 maxAlpha = dgFloat32 (0.0f);
		dgFloat32 maxSpeed = dgFloat32 (0.0f);
		dgFloat32 maxOmega = dgFloat32 (0.0f);

		const dgFloat32 speedFreeze = world->m_freezeSpeed2;
		const dgFloat32 accelFreeze = world->m_freezeAccel2;
		const dgVector forceDampVect (forceDamp, forceDamp, forceDamp, dgFloat32 (0.0f));
		for (dgInt32 i = 1; i < bodyCount; i ++) {
			dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
			if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
				dgAssert (body->m_invMass.m_w);

				const dgFloat32 accel2 = body->m_accel.DotProduct3(body->m_accel);
				const dgFloat32 alpha2 = body->m_alpha.DotProduct3(body->m_alpha);
				const dgFloat32 speed2 = body->m_veloc.DotProduct3(body->m_veloc);
				const dgFloat32 omega2 = body->m_omega.DotProduct3(body->m_omega);

				maxAccel = dgMax (maxAccel, accel2);
				maxAlpha = dgMax (maxAlpha, alpha2);
				maxSpeed = dgMax (maxSpeed, speed2);
				maxOmega = dgMax (maxOmega, omega2);

				bool equilibrium = (accel2 < accelFreeze) && (alpha2 < accelFreeze) && (speed2 < speedFreeze) && (omega2 < speedFreeze);
				if (equilibrium) {
					dgVector veloc (body->m_veloc * forceDampVect);
					dgVector omega = body->m_omega * forceDampVect;
					body->m_veloc = (dgVector (veloc.DotProduct4(veloc)) > m_velocTol) & veloc;
					body->m_omega = (dgVector (omega.DotProduct4(omega)) > m_velocTol) & omega;

				}
				body->m_equilibrium = dgUnsigned32 (equilibrium);
				stackSleeping &= equilibrium;
				isAutoSleep &= body->m_autoSleep;

				sleepCounter = dgMin (sleepCounter, body->m_sleepingCounter);
			}
			// clear accel and angular acceleration
			body->m_accel = dgVector::m_zero;
			body->m_alpha = dgVector::m_zero;
		}

		if (isAutoSleep) {
			if (stackSleeping) {
				// the island went to sleep mode, 
				for (dgInt32 i = 1; i < bodyCount; i ++) {
					dgBody* const body = bodyArray[i].m_body;
					dgAssert (body->IsRTTIType (dgBody::m_dynamicBodyRTTI) || body->IsRTTIType (dgBody::m_kinematicBodyRTTI));
					body->m_accel = dgVector::m_zero;
					body->m_alpha = dgVector::m_zero;
					body->m_veloc = dgVector::m_zero;
					body->m_omega = dgVector::m_zero;
				}
			} else {
				// island is not sleeping but may be resting with small residual velocity for a long time
				// see if we can force to go to sleep
				if ((maxAccel > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAccel) ||
					(maxAlpha > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAlpha) ||
					(maxSpeed > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxVeloc) ||
					(maxOmega > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxOmega)) {
					for (dgInt32 i = 1; i < bodyCount; i ++) {
						dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
						if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
							body->m_sleepingCounter = 0;
						}
					}
				} else {
					dgInt32 index = 0;
					for (dgInt32 i = 0; i < DG_SLEEP_ENTRIES; i ++) {
						if ((maxAccel <= world->m_sleepTable[i].m_maxAccel) &&
							(maxAlpha <= world->m_sleepTable[i].m_maxAlpha) &&
							(maxSpeed <= world->m_sleepTable[i].m_maxVeloc) &&
							(maxOmega <= world->m_sleepTable[i].m_maxOmega)) {
								index = i;
								break;
						}
					}

					dgInt32 timeScaleSleepCount = dgInt32 (dgFloat32 (60.0f) * sleepCounter * timestep);
					if (timeScaleSleepCount > world->m_sleepTable[index].m_steps) {
						// force island to sleep
						stackSleeping = true;
						for (dgInt32 i = 1; i < bodyCount; i ++) {
							dgBody* const body = bodyArray[i].m_body;
							dgAssert (body->IsRTTIType (dgBody::m_dynamicBodyRTTI) || body->IsRTTIType (dgBody::m_kinematicBodyRTTI));
							body->m_accel = dgVector::m_zero;
							body->m_alpha = dgVector::m_zero;
							body->m_veloc = dgVector::m_zero;
							body->m_omega = dgVector::m_zero;
							body->m_equilibrium = true;
						}
					} else {
						sleepCounter ++;
						for (dgInt32 i = 1; i < bodyCount; i ++) {
							dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
							if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
								body->m_sleepingCounter = sleepCounter;
							}
						}
					}
				}
			}
		} 


		if (!(isAutoSleep & stackSleeping)) {
			// island is not sleeping, need to integrate island velocity
			const dgUnsigned32 lru = world->GetBroadPhase()->m_lru;
			const dgInt32 jointCount = cluster->m_jointCount;
			dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
			dgJointInfo* const constraintArray = &constraintArrayPtr[cluster->m_jointStart];

			dgFloat32 timeRemaining = timestep;
			const dgFloat32 timeTol = dgFloat32 (0.01f) * timestep;
			for (dgInt32 i = 0; (i < DG_MAX_CONTINUE_COLLISON_STEPS) && (timeRemaining > timeTol); i ++) {
				// calculate the closest time to impact 
				dgFloat32 timeToImpact = timeRemaining;
				for (dgInt32 j = 0; (j < jointCount) && (timeToImpact > timeTol); j ++) {
					dgContact* const contact = (dgContact*) constraintArray[j].m_joint;
					if (contact->GetId() == dgConstraint::m_contactConstraint) {
						dgDynamicBody* const body0 = (dgDynamicBody*)contact->m_body0;
						dgDynamicBody* const body1 = (dgDynamicBody*)contact->m_body1;
						if (body0->m_continueCollisionMode | body1->m_continueCollisionMode) {
							dgVector p;
							dgVector q;
							dgVector normal;
							timeToImpact = dgMin (timeToImpact, world->CalculateTimeToImpact (contact, timeToImpact, threadID, p, q, normal, dgFloat32 (-1.0f / 256.0f)));
						}
					}
				}

				if (timeToImpact > timeTol) {
					timeRemaining -= timeToImpact;
					for (dgInt32 j = 1; j < bodyCount; j ++) {
						dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
						if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
							body->IntegrateVelocity(timeToImpact);
							body->UpdateWorlCollisionMatrix();
						}
					}
				} else {
					if (timeToImpact >= dgFloat32 (-1.0e-5f)) {
						for (dgInt32 j = 1; j < bodyCount; j++) {
							dgDynamicBody* const body = (dgDynamicBody*)bodyArray[j].m_body;
							if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
								body->IntegrateVelocity(timeToImpact);
								body->UpdateWorlCollisionMatrix();
							}
						}
					}

					CalculateClusterContacts (cluster, timeRemaining, lru, threadID);
					BuildJacobianMatrix (cluster, threadID, 0.0f);
					IntegrateReactionsForces (cluster, threadID, 0.0f, DG_SOLVER_MAX_ERROR);

					bool clusterReceding = true;
					const dgFloat32 step = timestep * dgFloat32 (1.0f / DG_MAX_CONTINUE_COLLISON_STEPS); 
					for (dgInt32 k = 0; (k < DG_MAX_CONTINUE_COLLISON_STEPS) && clusterReceding; k ++) {
						dgFloat32 smallTimeStep = dgMin (step, timeRemaining);
						timeRemaining -= smallTimeStep;
						for (dgInt32 j = 1; j < bodyCount; j ++) {
							dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
							if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
								body->IntegrateVelocity (smallTimeStep);
								body->UpdateWorlCollisionMatrix();
							}
						}

						clusterReceding = false;
						if (timeRemaining > timeTol) {
							CalculateClusterContacts (cluster, timeRemaining, lru, threadID);

							bool isColliding = false;
							for (dgInt32 j = 0; (j < jointCount) && !isColliding; j ++) {
								dgContact* const contact = (dgContact*) constraintArray[j].m_joint;
								if (contact->GetId() == dgConstraint::m_contactConstraint) {

									const dgBody* const body0 = contact->m_body0;
									const dgBody* const body1 = contact->m_body1;

									const dgVector& veloc0 = body0->m_veloc;
									const dgVector& veloc1 = body1->m_veloc;

									const dgVector& omega0 = body0->m_omega;
									const dgVector& omega1 = body1->m_omega;

									const dgVector& com0 = body0->m_globalCentreOfMass;
									const dgVector& com1 = body1->m_globalCentreOfMass;
									
									for (dgList<dgContactMaterial>::dgListNode* node = contact->GetFirst(); node; node = node->GetNext()) {
										const dgContactMaterial* const contactMaterial = &node->GetInfo();
										dgVector vel0 (veloc0 + omega0.CrossProduct3(contactMaterial->m_point - com0));
										dgVector vel1 (veloc1 + omega1.CrossProduct3(contactMaterial->m_point - com1));
										dgVector vRel (vel0 - vel1);
										dgAssert (contactMaterial->m_normal.m_w == dgFloat32 (0.0f));
										dgFloat32 speed = vRel.DotProduct4(contactMaterial->m_normal).m_w;
										isColliding |= (speed < dgFloat32 (0.0f));
									}
								}
							}
							clusterReceding = !isColliding;
						}
					}
				}
			}

			if (timeRemaining > dgFloat32 (0.0)) {
				for (dgInt32 j = 1; j < bodyCount; j ++) {
					dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
					if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
						body->IntegrateVelocity(timeRemaining);
						body->UpdateCollisionMatrix (timeRemaining, threadID);
					}
				}
			} else {
				for (dgInt32 j = 1; j < bodyCount; j ++) {
					dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
					if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
						body->UpdateCollisionMatrix (timestep, threadID);
					}
				}
			}
		}
	}
}


void dgWorldDynamicUpdate::CalculateClusterContacts(dgBodyCluster* const cluster, dgFloat32 timestep, dgInt32 currLru, dgInt32 threadID) const
{
	dgWorld* const world = (dgWorld*) this;
	dgInt32 jointCount = cluster->m_jointCount;
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[cluster->m_jointStart];

	dgBroadPhase::dgPair pair;
	dgContactPoint contactArray[DG_MAX_CONTATCS];
	for (dgInt32 j = 0; (j < jointCount); j ++) {
		dgContact* const contact = (dgContact*) constraintArray[j].m_joint;
		if (contact->GetId() == dgConstraint::m_contactConstraint) {
			const dgContactMaterial* const material = contact->m_material;
			if (material->m_flags & dgContactMaterial::m_collisionEnable) {
				dgInt32 processContacts = 1;
				if (material->m_aabbOverlap) {
					processContacts = material->m_aabbOverlap (*material, *contact->GetBody0(), *contact->GetBody1(), threadID);
				}

				if (processContacts) {
					contact->m_maxDOF = 0;
					contact->m_broadphaseLru = currLru;
					pair.m_contact = contact;
					pair.m_cacheIsValid = false;
					pair.m_timestep = timestep;
					pair.m_contactBuffer = contactArray;
					world->CalculateContacts (&pair, threadID, false, false);
					if (pair.m_contactCount) {
						dgAssert (pair.m_contactCount <= (DG_CONSTRAINT_MAX_ROWS / 3));
						world->ProcessContacts (&pair, threadID);
					}
				}
			}
		}
	}
}


void dgWorldDynamicUpdate::BuildJacobianMatrix (const dgBodyInfo* const bodyInfoArray, const dgJointInfo* const jointInfo, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow, dgFloat32 forceImpulseScale) const 
{
	const dgInt32 index = jointInfo->m_pairStart;
	const dgInt32 count = jointInfo->m_pairCount;
	const dgInt32 m0 = jointInfo->m_m0;
	const dgInt32 m1 = jointInfo->m_m1;

	const dgBody* const body0 = bodyInfoArray[m0].m_body;
	const dgBody* const body1 = bodyInfoArray[m1].m_body;
	const bool isBilateral = jointInfo->m_joint->IsBilateral();

	const dgVector invMass0(body0->m_invMass[3]);
	const dgMatrix& invInertia0 = body0->m_invWorldInertiaMatrix;
	const dgVector invMass1(body1->m_invMass[3]);
	const dgMatrix& invInertia1 = body1->m_invWorldInertiaMatrix;

	dgVector force0(dgVector::m_zero);
	dgVector torque0(dgVector::m_zero);
	if (body0->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
		force0 = ((dgDynamicBody*)body0)->m_externalForce;
		torque0 = ((dgDynamicBody*)body0)->m_externalTorque;
	}

	dgVector force1(dgVector::m_zero);
	dgVector torque1(dgVector::m_zero);
	if (body1->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
		force1 = ((dgDynamicBody*)body1)->m_externalForce;
		torque1 = ((dgDynamicBody*)body1)->m_externalTorque;
	}

	const dgVector scale0(jointInfo->m_scale0);
	const dgVector scale1(jointInfo->m_scale1);

	dgJacobian forceAcc0;
	dgJacobian forceAcc1;
	forceAcc0.m_linear = dgVector::m_zero;
	forceAcc0.m_angular = dgVector::m_zero;
	forceAcc1.m_linear = dgVector::m_zero;
	forceAcc1.m_angular = dgVector::m_zero;
	
	for (dgInt32 i = 0; i < count; i++) {
		dgJacobianMatrixElement* const row = &matrixRow[index + i];
		dgAssert(row->m_Jt.m_jacobianM0.m_linear.m_w == dgFloat32(0.0f));
		dgAssert(row->m_Jt.m_jacobianM0.m_angular.m_w == dgFloat32(0.0f));
		dgAssert(row->m_Jt.m_jacobianM1.m_linear.m_w == dgFloat32(0.0f));
		dgAssert(row->m_Jt.m_jacobianM1.m_angular.m_w == dgFloat32(0.0f));

		row->m_JMinv.m_jacobianM0.m_linear =  row->m_Jt.m_jacobianM0.m_linear * invMass0;
		row->m_JMinv.m_jacobianM0.m_angular = invInertia0.RotateVector(row->m_Jt.m_jacobianM0.m_angular);
		row->m_JMinv.m_jacobianM1.m_linear =  row->m_Jt.m_jacobianM1.m_linear * invMass1;
		row->m_JMinv.m_jacobianM1.m_angular = invInertia1.RotateVector(row->m_Jt.m_jacobianM1.m_angular);

		dgVector tmpAccel(row->m_JMinv.m_jacobianM0.m_linear * force0 + row->m_JMinv.m_jacobianM0.m_angular * torque0 +
						  row->m_JMinv.m_jacobianM1.m_linear * force1 + row->m_JMinv.m_jacobianM1.m_angular * torque1);

		dgAssert(tmpAccel.m_w == dgFloat32(0.0f));
		dgFloat32 extenalAcceleration = -(tmpAccel.AddHorizontal()).GetScalar();
		row->m_deltaAccel = extenalAcceleration * forceImpulseScale;
		row->m_coordenateAccel += extenalAcceleration * forceImpulseScale;
		dgAssert(row->m_jointFeebackForce);
		row->m_accel = row->m_jointFeebackForce->m_accel;
		const dgFloat32 force = row->m_jointFeebackForce->m_force * forceImpulseScale; 
		row->m_force = isBilateral ? dgClamp(force, row->m_lowerBoundFrictionCoefficent, row->m_upperBoundFrictionCoefficent) : force;
		row->m_force0 = row->m_force;
		row->m_maxImpact = dgFloat32(0.0f);

		dgVector jMinvM0linear (scale0 * row->m_JMinv.m_jacobianM0.m_linear);
		dgVector jMinvM0angular (scale0 * row->m_JMinv.m_jacobianM0.m_angular);
		dgVector jMinvM1linear (scale1 * row->m_JMinv.m_jacobianM1.m_linear);
		dgVector jMinvM1angular (scale1 * row->m_JMinv.m_jacobianM1.m_angular);

		dgVector tmpDiag(jMinvM0linear * row->m_Jt.m_jacobianM0.m_linear + jMinvM0angular * row->m_Jt.m_jacobianM0.m_angular +
						 jMinvM1linear * row->m_Jt.m_jacobianM1.m_linear + jMinvM1angular * row->m_Jt.m_jacobianM1.m_angular);

		dgAssert (tmpDiag.m_w == dgFloat32 (0.0f));
		dgFloat32 diag = (tmpDiag.AddHorizontal()).GetScalar();
		dgAssert(diag > dgFloat32(0.0f));
		row->m_diagDamp = diag * row->m_stiffness;
		diag *= (dgFloat32(1.0f) + row->m_stiffness);
		row->m_jMinvJt = diag;
		row->m_invJMinvJt = dgFloat32(1.0f) / diag;

		dgAssert(dgCheckFloat(row->m_accel));
		dgAssert(dgCheckFloat(row->m_force));
		dgVector val(row->m_force);
		forceAcc0.m_linear += row->m_Jt.m_jacobianM0.m_linear * val;
		forceAcc0.m_angular += row->m_Jt.m_jacobianM0.m_angular * val;
		forceAcc1.m_linear += row->m_Jt.m_jacobianM1.m_linear * val;
		forceAcc1.m_angular += row->m_Jt.m_jacobianM1.m_angular * val;
	}

	forceAcc0.m_linear = forceAcc0.m_linear * scale0;
	forceAcc0.m_angular = forceAcc0.m_angular * scale0;
	forceAcc1.m_linear = forceAcc1.m_linear * scale1;
	forceAcc1.m_angular = forceAcc1.m_angular * scale1;

	if (!body0->m_equilibrium) {
		internalForces[m0].m_linear += forceAcc0.m_linear;
		internalForces[m0].m_angular += forceAcc0.m_angular;
	}
	if (!body1->m_equilibrium) {
		internalForces[m1].m_linear += forceAcc1.m_linear;
		internalForces[m1].m_angular += forceAcc1.m_angular;
	}
}


void dgWorldDynamicUpdate::BuildJacobianMatrix(dgBodyCluster* const cluster, dgInt32 threadID, dgFloat32 timestep) const 
{
	dTimeTrackerEvent(__FUNCTION__);
	dgAssert (cluster->m_bodyCount >= 2);

	dgWorld* const world = (dgWorld*) this;
	const dgInt32 bodyCount = cluster->m_bodyCount;

	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart];
	dgJacobian* const internalForces = &m_solverMemory.m_internalForcesBuffer[cluster->m_bodyStart];

	dgAssert (((dgDynamicBody*) bodyArray[0].m_body)->IsRTTIType (dgBody::m_dynamicBodyRTTI));
	dgAssert ((((dgDynamicBody*)bodyArray[0].m_body)->m_accel.DotProduct3(((dgDynamicBody*)bodyArray[0].m_body)->m_accel)) == dgFloat32 (0.0f));
	dgAssert ((((dgDynamicBody*)bodyArray[0].m_body)->m_alpha.DotProduct3(((dgDynamicBody*)bodyArray[0].m_body)->m_alpha)) == dgFloat32 (0.0f));
	dgAssert ((((dgDynamicBody*)bodyArray[0].m_body)->m_externalForce.DotProduct3(((dgDynamicBody*)bodyArray[0].m_body)->m_externalForce)) == dgFloat32 (0.0f));
	dgAssert ((((dgDynamicBody*)bodyArray[0].m_body)->m_externalTorque.DotProduct3(((dgDynamicBody*)bodyArray[0].m_body)->m_externalTorque)) == dgFloat32 (0.0f));

	internalForces[0].m_linear = dgVector::m_zero;
	internalForces[0].m_angular = dgVector::m_zero;

	if (timestep != dgFloat32 (0.0f)) {
		for (dgInt32 i = 1; i < bodyCount; i ++) {
			dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
			//dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI));
			dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
			if (!body->m_equilibrium) {
				dgAssert (body->m_invMass.m_w > dgFloat32 (0.0f));
				body->AddDampingAcceleration(timestep);
				body->CalcInvInertiaMatrix ();
//				body->ApplyGyroTorque();
				internalForces[i].m_linear = dgVector::m_zero;
				internalForces[i].m_angular = dgVector::m_zero;
			} else if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
				internalForces[i].m_linear = body->m_externalForce * dgVector::m_negOne;
				internalForces[i].m_angular = body->m_externalTorque * dgVector::m_negOne;
			} else {
				internalForces[i].m_linear = dgVector::m_zero;
				internalForces[i].m_angular = dgVector::m_zero;
			}

			// re use these variables for temp storage 
			body->m_accel = body->m_veloc;
			body->m_alpha = body->m_omega;
		}

	} else {
		for (dgInt32 i = 1; i < bodyCount; i ++) {
			dgBody* const body = bodyArray[i].m_body;
			dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
			if (!body->m_equilibrium) {
				dgAssert (body->m_invMass.m_w > dgFloat32 (0.0f));
				body->CalcInvInertiaMatrix ();
			}

			// re use these variables for temp storage 
			body->m_accel = body->m_veloc;
			body->m_alpha = body->m_omega;

			internalForces[i].m_linear = dgVector::m_zero;
			internalForces[i].m_angular = dgVector::m_zero;
		}
	}

	dgContraintDescritor constraintParams;

	constraintParams.m_world = world;
	constraintParams.m_threadIndex = threadID;
	constraintParams.m_timestep = timestep;
	constraintParams.m_invTimestep = (timestep > dgFloat32(1.0e-5f)) ? dgFloat32(1.0f / timestep) : dgFloat32(0.0f);
	const dgFloat32 forceOrImpulseScale = (timestep > dgFloat32(0.0f)) ? dgFloat32(1.0f) : dgFloat32(0.0f);

	dgJointInfo* const constraintArrayPtr = (dgJointInfo*)&world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[cluster->m_jointStart];
	dgJacobianMatrixElement* const matrixRow = &m_solverMemory.m_jacobianBuffer[cluster->m_rowsStart];

	dgInt32 rowCount = 0;
	//const dgInt32 jointCount = island->m_jointCount;
	const dgInt32 jointCount = cluster->m_activeJointCount;
	for (dgInt32 i = 0; i < jointCount; i++) {
		dgJointInfo* const jointInfo = &constraintArray[i];
		dgConstraint* const constraint = jointInfo->m_joint;

		dgAssert (dgInt32 (constraint->m_index) == i);
		dgAssert(jointInfo->m_m0 < cluster->m_bodyCount);
		dgAssert(jointInfo->m_m1 < cluster->m_bodyCount);
		//dgAssert (constraint->m_index == dgUnsigned32(j));

		rowCount = GetJacobianDerivatives(constraintParams, jointInfo, constraint, matrixRow, rowCount);
		dgAssert(rowCount <= cluster->m_rowsCount);

		dgAssert(jointInfo->m_m0 >= 0);
		dgAssert(jointInfo->m_m0 < bodyCount);
		dgAssert(jointInfo->m_m1 >= 0);
		dgAssert(jointInfo->m_m1 < bodyCount);
		BuildJacobianMatrix(bodyArray, jointInfo, internalForces, matrixRow, forceOrImpulseScale);
	}
}


void dgWorldDynamicUpdate::IntegrateExternalForce(const dgBodyCluster* const cluster, dgFloat32 timestep, dgInt32 threadID) const
{
	dgWorld* const world = (dgWorld*) this;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart];

	dgAssert (timestep > dgFloat32 (0.0f));
	const dgInt32 bodyCount = cluster->m_bodyCount;
	for (dgInt32 i = 1; i < bodyCount; i ++) {
		dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
		body->IntegrateOpenLoopExternalForce(timestep);
	}
}


void dgWorldDynamicUpdate::CalculateNetAcceleration(dgBody* const body, const dgVector& invTimeStep, const dgVector& maxAccNorm2) const
{
	dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
	// the initial velocity and angular velocity were stored in m_accel and body->m_alpha for memory saving
	dgVector accel (invTimeStep * (body->m_veloc - body->m_accel));
	dgVector alpha (invTimeStep * (body->m_omega - body->m_alpha));
	dgVector accelTest((accel.DotProduct4(accel) > maxAccNorm2) | (alpha.DotProduct4(alpha) > maxAccNorm2));
	accel = accel & accelTest;
	alpha = alpha & accelTest;

	body->m_accel = accel;
	body->m_alpha = alpha;
}


void dgWorldDynamicUpdate::IntegrateReactionsForces(const dgBodyCluster* const cluster, dgInt32 threadID, dgFloat32 timestep, dgFloat32 maxAccNorm) const
{
	if (cluster->m_jointCount == 0) {
		IntegrateExternalForce(cluster, timestep, threadID);
	} else {
		CalculateClusterReactionForces(cluster, threadID, timestep, maxAccNorm);
	}
}

dgFloat32 dgWorldDynamicUpdate::CalculateJointForceGaussSeidel(const dgJointInfo* const jointInfo, const dgBodyInfo* const bodyArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow, dgFloat32 restAcceleration) const
{
	dgVector accNorm(dgVector::m_zero);
	const dgInt32 m0 = jointInfo->m_m0;
	const dgInt32 m1 = jointInfo->m_m1;

	dgVector linearM0(internalForces[m0].m_linear);
	dgVector angularM0(internalForces[m0].m_angular);
	dgVector linearM1(internalForces[m1].m_linear);
	dgVector angularM1(internalForces[m1].m_angular);
	const dgVector scale0(jointInfo->m_scale0);
	const dgVector scale1(jointInfo->m_scale1);

	const dgInt32 index = jointInfo->m_pairStart;
	const dgInt32 rowsCount = jointInfo->m_pairCount;

	dgFloat32 cacheForce[DG_CONSTRAINT_MAX_ROWS + 4];
	cacheForce[0] = dgFloat32(1.0f);
	cacheForce[1] = dgFloat32(1.0f);
	cacheForce[2] = dgFloat32(1.0f);
	cacheForce[3] = dgFloat32(1.0f);
	dgFloat32* const normalForce = &cacheForce[4];

	dgVector maxAccel(1.0e10f);
	dgFloat32 prevError = dgFloat32(1.0e20f);
	dgVector firstPass (dgVector::m_one);
	for (dgInt32 j = 0; (j < 5) && (maxAccel.GetScalar() > restAcceleration) && (prevError - maxAccel.GetScalar()) > dgFloat32(1.0e-2f); j++) {
		prevError = maxAccel.GetScalar();
		maxAccel = dgVector::m_zero;
		for (dgInt32 i = 0; i < rowsCount; i++) {
			dgJacobianMatrixElement* const row = &matrixRow[index + i];

			dgAssert(row->m_Jt.m_jacobianM0.m_linear.m_w == dgFloat32(0.0f));
			dgAssert(row->m_Jt.m_jacobianM0.m_angular.m_w == dgFloat32(0.0f));
			dgAssert(row->m_Jt.m_jacobianM1.m_linear.m_w == dgFloat32(0.0f));
			dgAssert(row->m_Jt.m_jacobianM1.m_angular.m_w == dgFloat32(0.0f));

			dgVector diag(row->m_JMinv.m_jacobianM0.m_linear * linearM0 + row->m_JMinv.m_jacobianM0.m_angular * angularM0 +
						  row->m_JMinv.m_jacobianM1.m_linear * linearM1 + row->m_JMinv.m_jacobianM1.m_angular * angularM1);

			dgVector accel(row->m_coordenateAccel - row->m_force * row->m_diagDamp - (diag.AddHorizontal()).GetScalar());
			dgVector force(row->m_force + row->m_invJMinvJt * accel.GetScalar());

			const dgInt32 frictionIndex = row->m_normalForceIndex;
			dgAssert(((frictionIndex < 0) && (normalForce[frictionIndex] == dgFloat32(1.0f))) || ((frictionIndex >= 0) && (normalForce[frictionIndex] >= dgFloat32(0.0f))));
			const dgFloat32 frictionNormal = normalForce[frictionIndex];
			dgVector lowerFrictionForce(frictionNormal * row->m_lowerBoundFrictionCoefficent);
			dgVector upperFrictionForce(frictionNormal * row->m_upperBoundFrictionCoefficent);

			accel = accel.AndNot((force > upperFrictionForce) | (force < lowerFrictionForce));
			dgVector accelAbs (accel.Abs());
			maxAccel = maxAccel.GetMax(accelAbs);
			accNorm = accNorm.GetMax(accelAbs * firstPass);
			dgAssert(maxAccel.m_x >= dgAbsf(accel.m_x));

			dgFloat32 f = (force.GetMax(lowerFrictionForce).GetMin(upperFrictionForce)).GetScalar();
			dgVector deltaForce(f - row->m_force);

			row->m_force = f;
			normalForce[i] = f;
			row->m_accel = dgFloat32(0.0f);
			dgVector deltaforce0(scale0 * deltaForce);
			dgVector deltaforce1(scale1 * deltaForce);
			linearM0 += row->m_Jt.m_jacobianM0.m_linear * deltaforce0;
			angularM0 += row->m_Jt.m_jacobianM0.m_angular * deltaforce0;
			linearM1 += row->m_Jt.m_jacobianM1.m_linear * deltaforce1;
			angularM1 += row->m_Jt.m_jacobianM1.m_angular * deltaforce1;
		}
		firstPass = dgVector::m_zero;
	}

	for (dgInt32 i = 0; i < rowsCount; i++) {
		dgJacobianMatrixElement* const row = &matrixRow[index + i];
		row->m_maxImpact = dgMax (dgAbsf (row->m_force), row->m_maxImpact);
	}

	internalForces[m0].m_linear = linearM0;
	internalForces[m0].m_angular = angularM0;
	internalForces[m1].m_linear = linearM1;
	internalForces[m1].m_angular = angularM1;
	
	return accNorm.GetScalar();
}


dgFloat32 dgWorldDynamicUpdate::CalculateJointConjugateGradient(const dgJointInfo* const jointInfo, const dgBodyInfo* const bodyArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow, dgFloat32 restAcceleration) const
{
	DG_MSC_VECTOR_ALIGMENT
	struct GradientParam
	{
		dgFloat32 m_x0;
		dgFloat32 m_r0;
		dgFloat32 m_r1;
		dgFloat32 m_z0;
		dgFloat32 m_d0;
		dgFloat32 m_p0;
		dgFloat32 m_b;
		dgFloat32 m_invM;
		//dgFloat32 m_low;
		//dgFloat32 m_high;
		//dgFloat32 m_normalForce;
		dgInt32 m_activeRows;
	} DG_GCC_VECTOR_ALIGMENT;
	GradientParam buffer[DG_CONSTRAINT_MAX_ROWS + 4];

	buffer[0].m_x0 = dgFloat32(1.0f);
	buffer[1].m_x0 = dgFloat32(1.0f);
	buffer[2].m_x0 = dgFloat32(1.0f);
	buffer[3].m_x0 = dgFloat32(1.0f);
	GradientParam* const param = &buffer[4];

static int xxx;
xxx++;
	const dgInt32 index = jointInfo->m_pairStart;
	dgInt32 rowsCount = jointInfo->m_pairCount;
	dgAssert(rowsCount <= DG_CONSTRAINT_MAX_ROWS);

	dgFloat32 accelNorm = dgFloat32(0.0f);
	const dgInt32 m0 = jointInfo->m_m0;
	const dgInt32 m1 = jointInfo->m_m1;
	dgJacobian y0 (internalForces[m0]);
	dgJacobian y1 (internalForces[m1]);

	dgInt32 normalCount = 0;
	for (dgInt32 i = 0; i < rowsCount; i++) {
		const dgJacobianMatrixElement* const row = &matrixRow[index + i];
		const dgJacobian JMinvM0 = row->m_JMinv.m_jacobianM0;
		const dgJacobian JMinvM1 = row->m_JMinv.m_jacobianM1;

		GradientParam& data = param[i];
		dgVector diag(JMinvM0.m_linear * y0.m_linear + JMinvM0.m_angular * y0.m_angular + JMinvM1.m_linear * y1.m_linear + JMinvM1.m_angular * y1.m_angular);
		dgFloat32 accel = row->m_coordenateAccel + row->m_accel - row->m_force * row->m_diagDamp - (diag.AddHorizontal()).GetScalar();
		accelNorm += accel * accel;

		const dgInt32 frictionIndex = row->m_normalForceIndex;
		normalCount += frictionIndex < 0;
		dgAssert(((frictionIndex < 0) && (param[frictionIndex].m_x0 == dgFloat32(1.0f))) || ((frictionIndex >= 0) && (param[frictionIndex].m_x0 >= dgFloat32(0.0f))));
	
		const dgVector f(row->m_force);
		const dgFloat32 frictionNormal = param[frictionIndex].m_x0;
		const dgVector low(frictionNormal * row->m_lowerBoundFrictionCoefficent);
		const dgVector high(frictionNormal * row->m_upperBoundFrictionCoefficent);
		const dgVector force(f.GetMax(low).GetMin(high));

		data.m_x0 = force.GetScalar();
		data.m_invM = dgFloat32(1.0f);
		data.m_d0 = row->m_diagDamp;
		data.m_b = row->m_coordenateAccel;
		data.m_activeRows = i;
	}

	if (accelNorm > dgFloat32(1.0e-6f)) {
		y0.m_linear = dgVector::m_zero;
		y0.m_angular = dgVector::m_zero;
		y1.m_linear = dgVector::m_zero;
		y1.m_angular = dgVector::m_zero;

		for (dgInt32 i = 0; i < rowsCount; i++) {
			const GradientParam& data = param[i];
			const dgJacobianMatrixElement* const row = &matrixRow[index + i];
			dgVector force(data.m_x0);
			y0.m_linear += row->m_Jt.m_jacobianM0.m_linear * force;
			y0.m_angular += row->m_Jt.m_jacobianM0.m_angular * force;
			y1.m_linear += row->m_Jt.m_jacobianM1.m_linear * force;
			y1.m_angular += row->m_Jt.m_jacobianM1.m_angular * force;
		}

		for (dgInt32 i = 0; i < rowsCount; i++) {
			GradientParam& data = param[i];
			const dgJacobianMatrixElement* const row = &matrixRow[index + i];
			dgVector accel(row->m_JMinv.m_jacobianM0.m_linear * y0.m_linear + row->m_JMinv.m_jacobianM0.m_angular * y0.m_angular +
						   row->m_JMinv.m_jacobianM1.m_linear * y1.m_linear + row->m_JMinv.m_jacobianM1.m_angular * y1.m_angular);

			data.m_r0 = data.m_b - data.m_x0 * data.m_d0 - (accel.AddHorizontal()).GetScalar();
			data.m_z0 = data.m_invM * data.m_r0;
			data.m_p0 = data.m_z0;
		}

		for (dgInt32 k = 0; k < 10; k++) {
xxx++;

			y0.m_linear = dgVector::m_zero;
			y0.m_angular = dgVector::m_zero;
			y1.m_linear = dgVector::m_zero;
			y1.m_angular = dgVector::m_zero;
			for (dgInt32 i = 0; i < rowsCount; i++) {
				const GradientParam& data = param[i];
				const dgInt32 j = data.m_activeRows;
				const dgJacobianMatrixElement* const row = &matrixRow[index + j];
				const dgVector force(data.m_p0);
				y0.m_linear += row->m_Jt.m_jacobianM0.m_linear * force;
				y0.m_angular += row->m_Jt.m_jacobianM0.m_angular * force;
				y1.m_linear += row->m_Jt.m_jacobianM1.m_linear * force;
				y1.m_angular += row->m_Jt.m_jacobianM1.m_angular * force;
			}


			dgFloat32 den = dgFloat32(0.0f);
			dgFloat32 num = dgFloat32(0.0f);
			for (dgInt32 i = 0; i < rowsCount; i++) {
				GradientParam& data = param[i];
				const dgInt32 j = data.m_activeRows;
				const dgJacobianMatrixElement* const row = &matrixRow[index + j];
				dgVector accel(row->m_JMinv.m_jacobianM0.m_linear * y0.m_linear + row->m_JMinv.m_jacobianM0.m_angular * y0.m_angular +
							   row->m_JMinv.m_jacobianM1.m_linear * y1.m_linear + row->m_JMinv.m_jacobianM1.m_angular * y1.m_angular);

				data.m_r1 = (accel.AddHorizontal()).GetScalar() + data.m_p0 * data.m_d0;
				den += data.m_r1 * data.m_p0;
				num += data.m_r0 * data.m_z0;
			}

//			dgFloat32 alpha = num / den;
//			bool needRestart = false;
/*
			for (dgInt32 i = 0; i < rowsCount; i++) {
				dgFloat32 f1 = x0[i] + alpha * p0[i];
				if (f1 < low[i]) {
					needRestart = true;
					x0[i] = low[i];
					dgSwap(activeRows[i], activeRows[rowsCount - 1]);
					i--;
					rowsCount--;
				} else if (f1 > high[i]) {
					needRestart = true;
					x0[i] = high[i];
					dgSwap(activeRows[i], activeRows[rowsCount - 1]);
					i--;
					rowsCount--;
				}
			}

			if (needRestart) {
				y0.m_linear = dgVector::m_zero;
				y0.m_angular = dgVector::m_zero;
				y1.m_linear = dgVector::m_zero;
				y1.m_angular = dgVector::m_zero;
				for (dgInt32 i = 0; i < rowsCount; i++) {
					const dgInt32 j = activeRows[i];
					const dgJacobianMatrixElement* const row = &matrixRow[index + j];
					dgVector force(x0[i]);
					y0.m_linear += row->m_Jt.m_jacobianM0.m_linear * force;
					y0.m_angular += row->m_Jt.m_jacobianM0.m_angular * force;
					y1.m_linear += row->m_Jt.m_jacobianM1.m_linear * force;
					y1.m_angular += row->m_Jt.m_jacobianM1.m_angular * force;
				}

				for (dgInt32 i = 0; i < rowsCount; i++) {
					const dgInt32 j = activeRows[i];
					const dgJacobianMatrixElement* const row = &matrixRow[index + j];
					dgVector accel(row->m_JMinv.m_jacobianM0.m_linear * y0.m_linear + row->m_JMinv.m_jacobianM0.m_angular * y0.m_angular +
						row->m_JMinv.m_jacobianM1.m_linear * y1.m_linear + row->m_JMinv.m_jacobianM1.m_angular * y1.m_angular);

					r0[i] = b[i] - x0[i] * d0[i] - (accel.AddHorizontal()).GetScalar();
					z0[i] = invM[i] * r0[i];
					p0[i] = z0[i];
				}

			} else {
				dgFloat32 betaNum = dgFloat32(0.0f);
				for (dgInt32 i = 0; i < rowsCount; i++) {
					x0[i] += alpha * p0[i];
					r0[i] -= alpha * r1[i];
					z0[i] = invM[i] * r0[i];
					betaNum += z0[i] * r0[i];
				}
				dgFloat32 beta = betaNum / num;
				if (beta < dgFloat32(1.0e-8f)) {
					break;
				}

				for (dgInt32 i = 0; i < rowsCount; i++) {
					p0[i] = r0[i] + beta * p0[i];
				}
			}
*/
		}

/*
		y0 = internalForces[m0];
		y1 = internalForces[m1];
		rowsCount = jointInfo->m_pairCount;
		for (dgInt32 i = 0; i < rowsCount; i++) {
			const dgInt32 j = activeRows[i];
			dgJacobianMatrixElement* const row = &matrixRow[index + j];
			dgVector jointForce(x0[i] - row->m_force);
			row->m_force = x0[i];
			y0.m_linear += row->m_Jt.m_jacobianM0.m_linear * jointForce;
			y0.m_angular += row->m_Jt.m_jacobianM0.m_angular * jointForce;
			y1.m_linear += row->m_Jt.m_jacobianM1.m_linear * jointForce;
			y1.m_angular += row->m_Jt.m_jacobianM1.m_angular * jointForce;
		}

		internalForces[m0] = y0;
		internalForces[m1] = y1;
*/
	}

	return accelNorm;
}



class dgDanzigSolver
{
	public:
	dgDanzigSolver()
	{
	}

	void SetSize(dgInt32 size)
	{
		m_size = size;
	}

	dgInt32 GetSize() const
	{
		return m_size;
	}

	dgFloat32* GetX()
	{
		return m_x;
	}

	dgFloat32* GetB()
	{
		return m_b;
	}

	dgFloat32* GetLow()
	{
		return m_low;
	}

	dgFloat32* GetHigh()
	{
		return m_high;
	}

	dgInt16* GetFrictionIndex()
	{
		return m_frictionIndex;
	}

	dgFloat32* GetMatrixRow(dgInt32 i)
	{
		return &m_matrix[i * m_size];
	}

	bool CholeskyFactorization()
	{
		const dgFloat32* src = m_matrix;
		dgFloat32* dst = m_choleskyMatrix;
		for (dgInt32 i = 0; i < m_size; i++) {
			for (dgInt32 j = 0; j < m_size; j++) {
				dst[j] = src[j];
			}
			dst += m_size;
			src += m_size;
		}

		dgFloat32* rowN = m_choleskyMatrix;
		for (dgInt32 i = 0; i < m_size; i++) {
			dgInt32 stride = 0;
			for (dgInt32 j = 0; j <= i; j++) {
				dgFloat32 s = dgFloat32(0.0f);
				dgFloat32* const rowJ = &m_choleskyMatrix[stride];
				for (dgInt32 k = 0; k < j; k++) {
					s += rowN[k] * rowJ[k];
				}

				if (i == j) {
					dgFloat32 diag = rowN[i] - s;
					if (diag < dgFloat32(1.0e-6f)) {
						return false;
					}
					rowN[i] = dgSqrt(diag);
				} else {
					rowN[j] = (rowN[j] - s) / rowJ[j];
				}
				stride += m_size;
			}
			rowN += m_size;
		}

		dst = m_choleskyMatrix;
		for (dgInt32 i = 0; i < m_size - 1; i++) {
			for (dgInt32 j = i + 1; j < m_size; j++) {
				dst[j] = dgFloat32 (0.0f);
			}
			dst += m_size;
		}
		return true;
	}

	void Solve()
	{
		CholeskyFactorization();


		dgInt32 index = 0;
		dgInt32 count = m_size;
		dgInt32 clampedIndex = m_size;
		dgInt32 initialGuessCount = m_size;

		m_x0[m_size] = m_x[m_size];
		for (dgInt32 i = 0; i < m_size; i++) {
			m_permute[i] = short(i);
			m_r0[i] = -m_b[i];
			m_x0[i] = dgFloat32(0.0f);
		}
		for (dgInt32 i = 0; i < m_size; i++) {
			m_delta_r[i] = -m_r0[i];
			m_delta_x[i] = m_delta_r[i];
		}
		for (dgInt32 test = true; test; ) {
			test = false;
			SolveCholesky(initialGuessCount, m_delta_x);
			dgInt32 permuteStart = initialGuessCount;

			dgFloat32 alpha = dgFloat32(1.0f);
			for (dgInt32 i = 0; i < initialGuessCount; i++) {
				const dgFloat32 x = m_x0[i] + alpha * m_delta_x[i];
				const dgInt32 frictionIndex = m_frictionIndex[i];
				const dgFloat32 low = m_low[i] * m_x0[frictionIndex];
				const dgFloat32 high = m_high[i] * m_x0[frictionIndex];
				if (x < low) {
					test = true;
					permuteStart = dgMin(permuteStart, i);
					dgFloat32 alpha1 = (low - m_x0[i]) / m_delta_x[i];
					dgAssert(alpha1 < alpha);
					dgAssert(alpha1 >= dgFloat32(0.0f));
					alpha = alpha1;
					initialGuessCount--;
		dgAssert(0);
					PermuteRows(i, initialGuessCount);
					i--;
				} else if (x > high) {
					test = true;
					permuteStart = dgMin(permuteStart, i);
					
					dgFloat32 alpha1 = (high - m_x0[i]) / m_delta_x[i];
					dgAssert(alpha1 < alpha);
					dgAssert(alpha1 >= dgFloat32(0.0f));
					alpha = alpha1;
					initialGuessCount--;
					PermuteRows(i, initialGuessCount);
					i--;
				}
			}

			for (dgInt32 i = 0; i < m_size; i++) {
				m_x0[i] += alpha * m_delta_x[i];
				m_r0[i] += alpha * m_delta_r[i];
				m_delta_r[i] = -m_r0[i];
				m_delta_x[i] = m_delta_r[i];
			}

			for (dgInt32 i = initialGuessCount; i < m_size; i++) {
				m_delta_x[i] = dgFloat32(0.0f);
			}

			if (test) {
				CholeskyUpdate(permuteStart, m_size - 1);
			}
		}

		count = m_size - initialGuessCount;
		if (count) {
			for (dgInt32 i = 0; i < m_size; i++) {
				m_r0[i] = dgFloat32(0.0f);
				m_delta_x[i] = dgFloat32(0.0f);
				m_delta_r[i] = dgFloat32(0.0f);
			}

			dgInt32 stride = initialGuessCount * m_size;
			for (dgInt32 j = initialGuessCount; j < m_size; j++) {
				dgFloat32 val = -m_b[j];
				const dgFloat32* const A = &m_matrix[stride];
				for (dgInt32 i = 0; i < m_size; i++) {
					val = val + A[i] * m_x0[i];
				}
				m_r0[j] = val;
				stride += m_size;
			}
			index = initialGuessCount;
		}

		while (count) {
			bool loop = true;
			bool calculateDelta_x = true;

			while (loop) {

				loop = false;
				dgFloat32 clamp_x = dgFloat32(0.0f);
				dgInt32 swapIndex = -1;

				if (dgAbsf(m_r0[index]) > dgFloat32(1.0e-12f)) {
					dgAssert(calculateDelta_x);

					if (calculateDelta_x) {
						const dgFloat32* const row = &m_matrix[m_size * index];
						for (dgInt32 i = 0; i < index; i++) {
							m_delta_x[i] = -row[i];
						}
						SolveCholesky(index, m_delta_x);
						m_delta_x[index] = dgFloat32(1.0f);
					}

					calculateDelta_x = true;
					dgInt32 stride = index * m_size;
					for (dgInt32 j = index; j < m_size; j++) {
						dgFloat32 val = dgFloat32(0.0f);
						const dgFloat32* const A = &m_matrix[stride];
						for (dgInt32 i = 0; i < m_size; i++) {
							val = val + A[i] * m_delta_x[i];
						}
						m_delta_r[j] = val;
						stride += m_size;
					}

					dgAssert(m_delta_r[index] != dgFloat32(0.0f));
					dgAssert(dgAbsf(m_delta_x[index]) == dgFloat32(1.0f));
					m_delta_r[index] = (m_delta_r[index] == dgFloat32(0.0f)) ? dgFloat32(1.0e-12f) : m_delta_r[index];

					dgFloat32 s = -m_r0[index] / m_delta_r[index];
					dgAssert(dgAbsf(s) >= dgFloat32(0.0f));

					for (dgInt32 i = 0; i <= index; i++) {
						dgFloat32 x1 = m_x0[i] + s * m_delta_x[i];
						const dgInt32 frictionIndex = m_frictionIndex[i];
						const dgFloat32 low = m_low[i] * m_x0[frictionIndex];
						const dgFloat32 high = m_high[i] * m_x0[frictionIndex];

						if (x1 > high) {
							swapIndex = i;
							clamp_x = high;
							s = (high - m_x0[i]) / m_delta_x[i];
						} else if (x1 < low) {
							swapIndex = i;
							clamp_x = low;
							s = (low - m_x0[i]) / m_delta_x[i];
						}
					}
					dgAssert(dgAbsf(s) >= dgFloat32(0.0f));

					for (dgInt32 i = clampedIndex; (i < m_size) && (s > dgFloat32(1.0e-12f)); i++) {
						dgFloat32 r1 = m_r0[i] + s * m_delta_r[i];
						if ((r1 * m_r0[i]) < dgFloat32(0.0f)) {
							dgAssert(dgAbsf(m_delta_r[i]) > dgFloat32(0.0f));
							dgFloat32 s1 = -m_r0[i] / m_delta_r[i];
							dgAssert(dgAbsf(s1) >= dgFloat32(0.0f));
							dgAssert(dgAbsf(s1) <= dgAbsf(s));
							if (dgAbsf(s1) < dgAbsf(s)) {
								s = s1;
								swapIndex = i;
							}
						}
					}

					for (dgInt32 i = 0; i < m_size; i++) {
						//dgAssert((m_x0[i] + dgAbsf(m_x0[i]) * dgFloat32(1.0e-4f)) >= m_low[i]);
						//dgAssert((m_x0[i] - dgAbsf(m_x0[i]) * dgFloat32(1.0e-4f)) <= m_high[i]);

						m_x0[i] += s * m_delta_x[i];
						m_r0[i] += s * m_delta_r[i];

						//dgAssert((m_x0[i] + dgFloat32(1.0f)) >= m_low[i]);
						//dgAssert((m_x0[i] - dgFloat32(1.0f)) <= m_high[i]);
					}
				}

				if (swapIndex == -1) {
					m_r0[index] = dgFloat32(0.0f);
					m_delta_r[index] = dgFloat32(0.0f);
					index++;
					count--;
					loop = false;
				} else if (swapIndex == index) {
					count--;
					clampedIndex--;
					m_x0[index] = clamp_x;
					PermuteRows(index, clampedIndex);
					CholeskyUpdate(index, clampedIndex);
					loop = count ? true : false;

				} else if (swapIndex > index) {
					loop = true;
					m_r0[swapIndex] = dgFloat32(0.0f);
					dgAssert(swapIndex < m_size);
					dgAssert(clampedIndex <= m_size);
					if (swapIndex < clampedIndex) {
						count--;
						clampedIndex--;
						PermuteRows(clampedIndex, swapIndex);
						CholeskyUpdate(swapIndex, clampedIndex);
						dgAssert(clampedIndex >= index);
					} else {
						count++;
						dgAssert(clampedIndex < m_size);
						PermuteRows(clampedIndex, swapIndex);
						CholeskyUpdate(clampedIndex, swapIndex);
						clampedIndex++;
						dgAssert(clampedIndex <= m_size);
						dgAssert(clampedIndex >= index);
					}
					calculateDelta_x = false;

				} else {
					dgAssert(index > 0);
					m_x0[swapIndex] = clamp_x;
					m_delta_x[index] = dgFloat32(0.0f);

					dgAssert(swapIndex < index);
					PermuteRows(swapIndex, index - 1);
					PermuteRows(index - 1, index);
					PermuteRows(clampedIndex - 1, index);
					CholeskyUpdate(swapIndex, clampedIndex - 1);

					clampedIndex--;
					index--;
					loop = true;
				}
			}
		}


		for (dgInt32 i = 0; i < m_size; i++) {
			dgInt32 j = m_permute[i];
			m_x[j] = m_x0[i];
			m_b[j] = m_r0[i];
		}
	}

	private:
//	static void dgPermuteRows(dgInt32 size, dgInt32 i, dgInt32 j, dgFloat32* const matrix, dgFloat32* const choleskyMatrix, dgFloat32* const x, dgFloat32* const r, dgFloat32* const low, dgFloat32* const high, dgInt16* const permute)
	void PermuteRows(dgInt32 i, dgInt32 j)
	{
		if (i != j) {
			dgFloat32* const A = &m_matrix[m_size * i];
			dgFloat32* const B = &m_matrix[m_size * j];
			dgFloat32* const invA = &m_choleskyMatrix[m_size * i];
			dgFloat32* const invB = &m_choleskyMatrix[m_size * j];
			for (dgInt32 k = 0; k < m_size; k++) {
				dgSwap(A[k], B[k]);
				dgSwap(invA[k], invB[k]);
			}

			dgInt32 stride = 0;
			for (dgInt32 k = 0; k < m_size; k++) {
				dgSwap(m_matrix[stride + i], m_matrix[stride + j]);
				stride += m_size;
			}

			dgSwap(m_b[i], m_b[j]);
			dgSwap(m_x0[i], m_x0[j]);
			dgSwap(m_r0[i], m_r0[j]);
			dgSwap(m_low[i], m_low[j]);
			dgSwap(m_high[i], m_high[j]);
			dgSwap(m_permute[i], m_permute[j]);
			dgSwap(m_frictionIndex[i], m_frictionIndex[j]);
		}
	}


//	void dgCholeskyUpdate(dgInt32 size, dgInt32 row, dgInt32 colum, dgFloat32* const choleskyMatrix, dgFloat32* const tmp, dgFloat32* const reflexion)
	void CholeskyUpdate(dgInt32 row, dgInt32 colum)
	{
		if (row != colum) {
			dgAssert(row < colum);

			// using Householder rotations, much more stable than Givens rotations
			for (dgInt32 i = row; i < m_size; i++) {
				dgFloat32* const rowI = &m_choleskyMatrix[m_size * i];
				m_activeColumns[0] = dgInt16(i);
				dgInt32 width = 1;
				for (dgInt32 j = i + 1; j < m_size; j++) {
					m_activeColumns[width] = dgInt16(j);
					width += dgAbsf(rowI[j]) > dgFloat32(1.0e-14f) ? 1 : 0;
				}

				if (width > 1) {
					dgFloat32 mag = dgFloat32(0.0f);
					for (dgInt32 j = 1; j < width; j++) {
						dgInt32 index = m_activeColumns[j];
						mag += rowI[index] * rowI[index];
						m_tmp1[index] = rowI[index];
					}
					m_tmp1[i] = rowI[i] - dgSqrt(mag + rowI[i] * rowI[i]);

					const dgFloat32 vMag2(mag + m_tmp1[i] * m_tmp1[i]);
					//const T den (dgFloat32 (1.0f) / T (sqrt (vMag2)));
					//for (dgInt32 j = 0; j < width; j ++) {
					//	dgInt32 index = activeColumns[j];
					//	reflexion[index] *= den;
					//}

					const dgFloat32 den = dgFloat32(2.0f) / vMag2;
					for (dgInt32 j = i; j < m_size; j++) {
						dgFloat32 acc = dgFloat32(0.0f);
						dgFloat32* const rowJ = &m_choleskyMatrix[m_size * j];
						for (dgInt32 k = 0; k < width; k++) {
							dgInt32 index = m_activeColumns[k];
							acc += rowJ[index] * m_tmp1[index];
						}
						m_tmp0[j] = acc;
					}

					for (dgInt32 j = i + 1; j < m_size; j++) {
						dgFloat32* const rowJ = &m_choleskyMatrix[m_size * j];
						const dgFloat32 a = m_tmp0[j] * den;
						for (dgInt32 k = 0; k < width; k++) {
							dgInt32 index = m_activeColumns[k];
							rowJ[index] -= a * m_tmp1[index];
						}
					}
					rowI[i] -= m_tmp0[i] * m_tmp1[i] * den;
				}

				for (dgInt32 k = i + 1; k < m_size; k++) {
					rowI[k] = dgFloat32(0.0f);
				}

				if (rowI[i] < dgFloat32(0.0f)) {
					for (dgInt32 k = i; k < m_size; k++) {
						m_choleskyMatrix[m_size * k + i] = -m_choleskyMatrix[m_size * k + i];
					}
				}
			}
			for (dgInt32 i = row; i < m_size; i++) {
				m_choleskyMatrix[m_size * i + i] = dgMax(m_choleskyMatrix[m_size * i + i], dgFloat32(1.0e-6f));
			}
		}
	}

//	void dgSolveCholesky(dgInt32 size, dgInt32 n, const dgFloat32* const choleskyMatrix, dgFloat32* const x)
	void SolveCholesky(dgInt32 n, dgFloat32* const out)
	{
		dgInt32 stride = 0;
		for (dgInt32 i = 0; i < n; i++) {
			dgFloat32 acc = dgFloat32(0.0f);
			const dgFloat32* const row = &m_choleskyMatrix[stride];
			for (dgInt32 j = 0; j < i; j++) {
				acc = acc + row[j] * out[j];
			}
			out[i] = (out[i] - acc) / row[i];
			stride += m_size;
		}

		for (dgInt32 i = n - 1; i >= 0; i--) {
			dgFloat32 acc = dgFloat32(0.0f);
			for (dgInt32 j = i + 1; j < n; j++) {
				acc = acc + m_choleskyMatrix[m_size * j + i] * out[j];
			}
			out[i] = (out[i] - acc) / m_choleskyMatrix[m_size * i + i];
		}
	}


	dgFloat32 m_x[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_b[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_x0[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_r0[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_low[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_high[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_tmp0[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_tmp1[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_delta_x[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_delta_r[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_matrix[DG_CONSTRAINT_MAX_ROWS * DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 m_choleskyMatrix[DG_CONSTRAINT_MAX_ROWS * DG_CONSTRAINT_MAX_ROWS];
	dgInt16 m_permute[DG_CONSTRAINT_MAX_ROWS];
	dgInt16 m_frictionIndex[DG_CONSTRAINT_MAX_ROWS];
	dgInt16 m_activeColumns[DG_CONSTRAINT_MAX_ROWS];
	dgInt32 m_size;
};



static void xxxxxx()
{
//	dgFloat32 angle = -45.00899f * 3.141592f / 180.0f;
	dgFloat32 angle = -50.0f * 3.141592f / 180.0f;
	dgMatrix matrix(dgRollMatrix(angle));
	dgVector r0(matrix.RotateVector(dgVector(-0.5f, -0.5f, 0.0f, 0.0f)));
	dgVector r1(matrix.RotateVector(dgVector(0.5f, -0.5f, 0.0f, 0.0f)));

	dgJacobian w;
	dgJacobian jt[4];
	dgDanzigSolver solver;

	solver.SetSize(4);
	dgFloat32* const b = solver.GetB();
	dgFloat32* const x = solver.GetX();
	dgFloat32* const m = solver.GetMatrixRow(0);
	dgFloat32* const low = solver.GetLow();
	dgFloat32* const high = solver.GetHigh();
	dgInt16* const frictionIndex = solver.GetFrictionIndex();

	jt[0].m_linear = matrix.RotateVector(dgVector(0.0f, 1.0f, 0.0f, 0.0f));
	jt[0].m_angular = r0.CrossProduct3(jt[0].m_linear);
	jt[1].m_linear = matrix.RotateVector(dgVector(0.0f, 1.0f, 0.0f, 0.0f));
	jt[1].m_angular = r1.CrossProduct3(jt[1].m_linear);
	jt[2].m_linear = matrix.RotateVector(dgVector(1.0f, 0.0f, 0.0f, 0.0f));
	jt[2].m_angular = r0.CrossProduct3(jt[2].m_linear);
	jt[3].m_linear = matrix.RotateVector(dgVector(1.0f, 0.0f, 0.0f, 0.0f));
	jt[3].m_angular = r1.CrossProduct3(jt[3].m_linear);

	low[0] = 0.0f;
	low[1] = 0.0f;
	low[2] = -0.9f;
	low[3] = -0.9f;
	high[0] = 1000.0f;
	high[1] = 1000.0f;
	high[2] = 0.9f;
	high[3] = 0.9f;

	frictionIndex[0] = dgInt16(solver.GetSize());
	frictionIndex[1] = dgInt16(solver.GetSize());
	frictionIndex[2] = 0;
	frictionIndex[3] = 1;

	x[solver.GetSize()] = dgFloat32(1.0f);


	w.m_linear = dgVector(0.0f, -10.0f, 0.0f, 0.0f);
	w.m_angular = dgVector(0.0f, 0.0f, 0.0f, 0.0f);

	for (dgInt32 i = 0; i < 4; i++) {
		dgVector tmp(jt[i].m_linear * w.m_linear + jt[i].m_angular * w.m_angular);
		b[i] = -tmp.AddHorizontal().GetScalar();
		for (dgInt32 j = 0; j < 4; j++) {
			dgVector acc(jt[i].m_linear * jt[j].m_linear + jt[i].m_angular * jt[j].m_angular);
			m[i * 4 + j] = acc.AddHorizontal().GetScalar();
		}
		m[i * 4 + i] *= 1.0001f;
	}
	solver.Solve();
//	xxxxxx1(4, &m[0][0], b, low, high);
}

dgFloat32 dgWorldDynamicUpdate::CalculateJointForceDanzig(const dgJointInfo* const jointInfo, const dgBodyInfo* const bodyArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow, dgFloat32 restAcceleration) const
{
	xxxxxx();
	return 0.0f;
/*
	dgFloat32 massMatrix[DG_CONSTRAINT_MAX_ROWS * DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 f[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 b[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 low[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 high[DG_CONSTRAINT_MAX_ROWS];
	dgFloat32 cacheForce[DG_CONSTRAINT_MAX_ROWS + 4];

	const dgInt32 m0 = jointInfo->m_m0;
	const dgInt32 m1 = jointInfo->m_m1;
	
	cacheForce[0] = dgFloat32(1.0f);
	cacheForce[1] = dgFloat32(1.0f);
	cacheForce[2] = dgFloat32(1.0f);
	cacheForce[3] = dgFloat32(1.0f);
	dgFloat32* const normalForce = &cacheForce[4];

	const dgInt32 index = jointInfo->m_pairStart;
	const dgInt32 rowsCount = jointInfo->m_pairCount;

	dgAssert (rowsCount <= DG_CONSTRAINT_MAX_ROWS);

	dgVector accNorm(dgVector::m_zero);
	for (dgInt32 i = 0; i < rowsCount; i++) {

		const dgJacobianMatrixElement* const row_i = &matrixRow[index + i];
		dgFloat32* const massMatrixRow = &massMatrix[rowsCount * i];

		dgJacobian JMinvM0(row_i->m_JMinv.m_jacobianM0);
		dgJacobian JMinvM1(row_i->m_JMinv.m_jacobianM1);
		dgVector diag(JMinvM0.m_linear * row_i->m_Jt.m_jacobianM0.m_linear + JMinvM0.m_angular * row_i->m_Jt.m_jacobianM0.m_angular +
					  JMinvM1.m_linear * row_i->m_Jt.m_jacobianM1.m_linear + JMinvM1.m_angular * row_i->m_Jt.m_jacobianM1.m_angular);

		diag = diag.AddHorizontal();
		dgFloat32 val = diag.GetScalar() + row_i->m_diagDamp;
		dgAssert(val > dgFloat32(0.0f));
		massMatrixRow[i] = val;

		for (dgInt32 j = i + 1; j < rowsCount; j++) {
			const dgJacobianMatrixElement* const row_j = &matrixRow[index + j];

			dgVector offDiag(JMinvM0.m_linear * row_j->m_Jt.m_jacobianM0.m_linear + JMinvM0.m_angular * row_j->m_Jt.m_jacobianM0.m_angular +
							 JMinvM1.m_linear * row_j->m_Jt.m_jacobianM1.m_linear + JMinvM1.m_angular * row_j->m_Jt.m_jacobianM1.m_angular);
			offDiag = offDiag.AddHorizontal();
			dgFloat32 val1 = offDiag.GetScalar();
			massMatrixRow[j] = val1;
			massMatrix[j * rowsCount + i] = val1;
		}
	}

	const dgJacobian& y0 = internalForces[m0];
	const dgJacobian& y1 = internalForces[m1];
	for (dgInt32 i = 0; i < rowsCount; i++) {
		const dgJacobianMatrixElement* const row = &matrixRow[index + i];

		dgJacobian JMinvM0(row->m_JMinv.m_jacobianM0);
		dgJacobian JMinvM1(row->m_JMinv.m_jacobianM1);
		dgVector diag(JMinvM0.m_linear * y0.m_linear + JMinvM0.m_angular * y0.m_angular +
					  JMinvM1.m_linear * y1.m_linear + JMinvM1.m_angular * y1.m_angular);
		dgVector accel(row->m_coordenateAccel - row->m_force * row->m_diagDamp - (diag.AddHorizontal()).GetScalar());
		
		dgInt32 frictionIndex = row->m_normalForceIndex;
		dgAssert(((frictionIndex < 0) && (normalForce[frictionIndex] == dgFloat32(1.0f))) || ((frictionIndex >= 0) && (normalForce[frictionIndex] >= dgFloat32(0.0f))));
		normalForce[i] = row->m_force;
		dgFloat32 frictionNormal = normalForce[frictionIndex];
		dgFloat32 lowerFrictionForce = frictionNormal * row->m_lowerBoundFrictionCoefficent;
		dgFloat32 upperFrictionForce = frictionNormal * row->m_upperBoundFrictionCoefficent;

		//f[i] = row->m_force;
		f[i] = dgFloat32 (0.0f);
		b[i] = accel.GetScalar();
		low[i] = lowerFrictionForce - row->m_force;
		high[i] = upperFrictionForce - row->m_force;

		dgVector force(row->m_force + row->m_invJMinvJt * accel.GetScalar());
		accel = accel.AndNot((force > upperFrictionForce) | (force < lowerFrictionForce));
		accNorm = accNorm.GetMax(accel.Abs());
	}

	dgSolveDantzigLCP(rowsCount, massMatrix, f, b, low, high);

	dgVector linearM0(internalForces[m0].m_linear);
	dgVector angularM0(internalForces[m0].m_angular);
	dgVector linearM1(internalForces[m1].m_linear);
	dgVector angularM1(internalForces[m1].m_angular);
	for (dgInt32 i = 0; i < rowsCount; i++) {
		dgJacobianMatrixElement* const row = &matrixRow[index + i];
		dgVector jointForce (f[i]);
		row->m_force = f[i] + row->m_force;
		linearM0 += row->m_Jt.m_jacobianM0.m_linear * jointForce;
		angularM0 += row->m_Jt.m_jacobianM0.m_angular * jointForce;
		linearM1 += row->m_Jt.m_jacobianM1.m_linear * jointForce;
		angularM1 += row->m_Jt.m_jacobianM1.m_angular * jointForce;
	}

	internalForces[m0].m_linear = linearM0;
	internalForces[m0].m_angular = angularM0;
	internalForces[m1].m_linear = linearM1;
	internalForces[m1].m_angular = angularM1;
	return accNorm.GetScalar();
*/
}


void dgWorldDynamicUpdate::CalculateClusterReactionForces(const dgBodyCluster* const cluster, dgInt32 threadID, dgFloat32 timestep, dgFloat32 maxAccNorm) const
{
	dTimeTrackerEvent(__FUNCTION__);
	dgWorld* const world = (dgWorld*) this;
	const dgInt32 bodyCount = cluster->m_bodyCount;
	//	const dgInt32 jointCount = island->m_jointCount;
	const dgInt32 jointCount = cluster->m_activeJointCount;

	dgJacobian* const internalForces = &m_solverMemory.m_internalForcesBuffer[cluster->m_bodyStart];
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*)&world->m_bodiesMemory[0];
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*)&world->m_jointsMemory[0];

	dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart];
	dgJointInfo* const constraintArray = &constraintArrayPtr[cluster->m_jointStart];
	dgJacobianMatrixElement* const matrixRow = &m_solverMemory.m_jacobianBuffer[cluster->m_rowsStart];

	const dgInt32 derivativesEvaluationsRK4 = 4;
	dgFloat32 invTimestep = (timestep > dgFloat32(0.0f)) ? dgFloat32(1.0f) / timestep : dgFloat32(0.0f);
	dgFloat32 invStepRK = (dgFloat32(1.0f) / dgFloat32(derivativesEvaluationsRK4));
	dgFloat32 timestepRK = timestep * invStepRK;
	dgFloat32 invTimestepRK = invTimestep * dgFloat32(derivativesEvaluationsRK4);
	dgAssert(bodyArray[0].m_body == world->m_sentinelBody);

	dgVector speedFreeze2(world->m_freezeSpeed2 * dgFloat32(0.1f));
	dgVector freezeOmega2(world->m_freezeOmega2 * dgFloat32(0.1f));

	dgJointAccelerationDecriptor joindDesc;
	joindDesc.m_timeStep = timestepRK;
	joindDesc.m_invTimeStep = invTimestepRK;
	joindDesc.m_firstPassCoefFlag = dgFloat32(0.0f);

	dgInt32 skeletonCount = 0;
	dgInt32 skeletonMemorySizeInBytes = 0;
	dgInt32 lru = dgAtomicExchangeAndAdd(&dgSkeletonContainer::m_lruMarker, 1);
	dgSkeletonContainer* skeletonArray[DG_MAX_SKELETON_JOINT_COUNT];
	dgInt32 memorySizes[DG_MAX_SKELETON_JOINT_COUNT];
	for (dgInt32 i = 1; i < bodyCount; i++) {
		dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
		dgSkeletonContainer* const container = body->GetSkeleton();
		if (container && (container->m_lru != lru)) {
			container->m_lru = lru;
			memorySizes[skeletonCount] = container->GetMemoryBufferSizeInBytes(constraintArray, matrixRow);
			skeletonMemorySizeInBytes += memorySizes[skeletonCount];
			skeletonArray[skeletonCount] = container;
			skeletonCount++;
			dgAssert(skeletonCount < dgInt32(sizeof(skeletonArray) / sizeof(skeletonArray[0])));
		}
	}

	dgInt8* const skeletonMemory = (dgInt8*)dgAlloca(dgVector, skeletonMemorySizeInBytes / sizeof(dgVector));
	dgAssert((dgInt64(skeletonMemory) & 0x0f) == 0);

	skeletonMemorySizeInBytes = 0;
	for (dgInt32 i = 0; i < skeletonCount; i++) {
		skeletonArray[i]->InitMassMatrix(constraintArray, matrixRow, &skeletonMemory[skeletonMemorySizeInBytes]);
		skeletonMemorySizeInBytes += memorySizes[i];
	}

	const dgInt32 passes = world->m_solverMode;
	for (dgInt32 step = 0; step < derivativesEvaluationsRK4; step++) {

		for (dgInt32 i = 0; i < jointCount; i++) {
			dgJointInfo* const jointInfo = &constraintArray[i];
			dgConstraint* const constraint = jointInfo->m_joint;
			joindDesc.m_rowsCount = jointInfo->m_pairCount;
			joindDesc.m_rowMatrix = &matrixRow[jointInfo->m_pairStart];
			constraint->JointAccelerations(&joindDesc);
		}
		joindDesc.m_firstPassCoefFlag = dgFloat32(1.0f);

		dgFloat32 accNorm(maxAccNorm * dgFloat32(2.0f));
		for (dgInt32 i = 0; (i < passes) && (accNorm > maxAccNorm); i++) {
			accNorm = dgFloat32(0.0f);
			for (dgInt32 j = 0; j < jointCount; j++) {
				dgJointInfo* const jointInfo = &constraintArray[j];
				dgFloat32 accel = CalculateJointForceGaussSeidel(jointInfo, bodyArray, internalForces, matrixRow, maxAccNorm);
//				dgFloat32 accel = CalculateJointConjugateGradient(jointInfo, bodyArray, internalForces, matrixRow, maxAccNorm);
//				dgFloat32 accel = CalculateJointForceDanzig(jointInfo, bodyArray, internalForces, matrixRow, maxAccNorm);
				accNorm = (accel > accNorm) ? accel : accNorm;
			}
		}
		for (dgInt32 j = 0; j < skeletonCount; j++) {
			skeletonArray[j]->CalculateJointForce(constraintArray, bodyArray, internalForces, matrixRow);
		}

		if (timestepRK != dgFloat32(0.0f)) {
			dgVector timestep4(timestepRK);
			for (dgInt32 i = 1; i < bodyCount; i++) {
				dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
				dgAssert(body->m_index == i);
				if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
					const dgJacobian& forceAndTorque = internalForces[i];
					dgVector force(body->m_externalForce + forceAndTorque.m_linear);
					dgVector torque(body->m_externalTorque + forceAndTorque.m_angular);

					dgVector velocStep((force.Scale4(body->m_invMass.m_w)) * timestep4);
					dgVector omegaStep((body->m_invWorldInertiaMatrix.RotateVector(torque)) * timestep4);
					body->m_veloc += velocStep;
					body->m_omega += omegaStep;

					dgAssert(body->m_veloc.m_w == dgFloat32(0.0f));
					dgAssert(body->m_omega.m_w == dgFloat32(0.0f));
				}
			}
		} else {
			for (dgInt32 i = 1; i < bodyCount; i++) {
				dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
				const dgVector& linearMomentum = internalForces[i].m_linear;
				const dgVector& angularMomentum = internalForces[i].m_angular;

				body->m_veloc += linearMomentum.Scale4(body->m_invMass.m_w);
				body->m_omega += body->m_invWorldInertiaMatrix.RotateVector(angularMomentum);
			}
		}
	}

	dgInt32 hasJointFeeback = 0;
	if (timestepRK != dgFloat32(0.0f)) {
		for (dgInt32 i = 0; i < jointCount; i++) {
			dgJointInfo* const jointInfo = &constraintArray[i];
			dgConstraint* const constraint = jointInfo->m_joint;

			const dgInt32 first = jointInfo->m_pairStart;
			const dgInt32 count = jointInfo->m_pairCount;

			for (dgInt32 j = 0; j < count; j++) {
				dgJacobianMatrixElement* const row = &matrixRow[j + first];
				dgAssert(dgCheckFloat(row->m_accel));
				dgAssert(dgCheckFloat(row->m_force));
				row->m_jointFeebackForce->m_force = row->m_force;
				row->m_jointFeebackForce->m_accel = row->m_accel;
				row->m_jointFeebackForce->m_impact = row->m_maxImpact * timestepRK;
			}
			hasJointFeeback |= (constraint->m_updaFeedbackCallback ? 1 : 0);
		}

		const dgVector invTime(invTimestep);
		const dgVector maxAccNorm2(maxAccNorm * maxAccNorm);
		for (dgInt32 i = 1; i < bodyCount; i++) {
			dgBody* const body = bodyArray[i].m_body;
			CalculateNetAcceleration(body, invTime, maxAccNorm2);
		}
		if (hasJointFeeback) {
			for (dgInt32 i = 0; i < jointCount; i++) {
				if (constraintArray[i].m_joint->m_updaFeedbackCallback) {
					constraintArray[i].m_joint->m_updaFeedbackCallback(*constraintArray[i].m_joint, timestep, threadID);
				}
			}
		}
	} else {
		for (dgInt32 i = 1; i < bodyCount; i++) {
			dgBody* const body = bodyArray[i].m_body;
			dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
			body->m_accel = dgVector::m_zero;
			body->m_alpha = dgVector::m_zero;
		}
	}
}


