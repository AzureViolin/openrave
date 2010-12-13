// -*- coding: utf-8 -*-
// Copyright (C) 2006-2010 Rosen Diankov (rosen.diankov@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#ifndef  WORKSPACE_TRAJECTORY_PLANNER_H
#define  WORKSPACE_TRAJECTORY_PLANNER_H

#include "rplanners.h"

class WorkspaceTrajectoryTracker : public PlannerBase
{
    class SetCustomFilterScope
    {
    public:
    SetCustomFilterScope(IkSolverBasePtr pik, const IkSolverBase::IkFilterCallbackFn& filterfn) : _pik(pik){
            _pik->SetCustomFilter(filterfn);
        }
        virtual ~SetCustomFilterScope() { _pik->SetCustomFilter(IkSolverBase::IkFilterCallbackFn()); }
    private:
        IkSolverBasePtr _pik;
    };

public:    
 WorkspaceTrajectoryTracker(EnvironmentBasePtr penv) : PlannerBase(penv)
    {
        __description = "\
:Interface Author:  Rosen Diankov\n\
Given a workspace trajectory of the end effector of a manipulator (active manipulator of the robot), finds a configuration space trajectory that tracks it using analytical inverse kinematics.\n\
Options can be specified to prioritize trajectory time, trajectory smoothness, and planning time\n\
In the simplest case, the workspace trajectory can be a straight line from one point to another.\n\
";
        _report.reset(new CollisionReport());
    }
    virtual ~WorkspaceTrajectoryTracker() {}

    virtual bool InitPlan(RobotBasePtr probot, PlannerParametersConstPtr params)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());

        boost::shared_ptr<WorkspaceTrajectoryParameters> parameters(new WorkspaceTrajectoryParameters(GetEnv()));
        parameters->copy(params);
        _robot = probot;
        _manip = _robot->GetActiveManipulator();

        if( (int)_manip->GetArmIndices().size() != parameters->GetDOF() ) {
            RAVELOG_WARN("parameter configuraiton space must be the robot's active manipulator\n");
            return false;
        }

        if( !parameters->_workspacetraj || parameters->_workspacetraj->GetTotalDuration() == 0 || parameters->_workspacetraj->GetPoints().size() == 0 ) {
            RAVELOG_ERROR("input trajectory needs to be initialized with interpolation information\n");
        }

        // check if the parameters configuration space actually reflects the active manipulator, move to the upper and lower limits
        {
            RobotBase::RobotStateSaver saver(_robot);
            boost::array<std::vector<dReal>*,2> testvalues = {{&parameters->_vConfigLowerLimit,&parameters->_vConfigUpperLimit}};
            vector<dReal> dummyvalues;
            for(size_t i = 0; i < testvalues.size(); ++i) {
                parameters->_setstatefn(*testvalues[i]);
                Transform tstate = _manip->GetEndEffectorTransform();
                _robot->SetActiveDOFs(_manip->GetArmIndices());
                _robot->GetActiveDOFValues(dummyvalues);
                for(size_t j = 0; j < dummyvalues.size(); ++j) {
                    if( RaveFabs(dummyvalues.at(j) - testvalues[i]->at(j)) > 2*g_fEpsilon ) {
                        RAVELOG_ERROR(str(boost::format("parameter configuration space does not match active manipulator, dof %d!\n")%j));
                        return false;
                    }
                }
            }
        }

        _fMaxCosDeviationAngle = RaveCos(parameters->_fMaxDeviationAngle);

        if( parameters->_bMaintainTiming ) {
            RAVELOG_WARN("currently do not support maintaining timing\n");
        }

        RobotBase::RobotStateSaver savestate(_robot);
        // should check collisio only for independent links that do not move during the planning process. This might require a CO_IndependentFromActiveDOFs option.
        //if(CollisionFunctions::CheckCollision(parameters,_robot,parameters->vinitialconfig, _report)) {
    
        // validate the initial state if one exists
        if( parameters->vinitialconfig.size() > 0 && !!parameters->_constraintfn ) {
            if( (int)parameters->vinitialconfig.size() != parameters->GetDOF() ) {
                RAVELOG_ERROR(str(boost::format("initial config wrong dim: %d\n")%parameters->vinitialconfig.size()));
                return false;
            }
            parameters->_setstatefn(parameters->vinitialconfig);
            if( !parameters->_constraintfn(parameters->vinitialconfig, parameters->vinitialconfig,0) ) {
                RAVELOG_WARN("initial state rejected by constraint fn\n");
                return false;
            }
        }

        if( !_manip->GetIkSolver() ) {
            RAVELOG_ERROR(str(boost::format("manipulator %s does not have ik solver set\n")%_manip->GetName()));
            return false;
        }
        
        _parameters = parameters;
        return true;
    }

    virtual bool PlanPath(TrajectoryBasePtr poutputtraj, boost::shared_ptr<std::ostream> pOutStream)
    {
        if(!_parameters) {
            RAVELOG_ERROR("WorkspaceTrajectoryTracker::PlanPath - Error, planner not initialized\n");
            return false;
        }
        
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        uint32_t basetime = timeGetTime();
        RobotBase::RobotStateSaver savestate(_robot);
        _robot->SetActiveDOFs(_manip->GetArmIndices()); // should be set by user anyway, but this is an extra precaution
        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);
        
        // first check if the end effectors are in collision
        TrajectoryBaseConstPtr workspacetraj = _parameters->_workspacetraj;
        TrajectoryBase::TPOINT pt;
        workspacetraj->SampleTrajectory(workspacetraj->GetTotalDuration(),pt);
        Transform tlasttrans = pt.trans;
        if( _manip->CheckEndEffectorCollision(tlasttrans,_report) ) {
            if( _parameters->_fMinimumCompleteTime >= workspacetraj->GetTotalDuration() ) {
                RAVELOG_DEBUG(str(boost::format("final configuration colliding: %s\n")%_report->__str__()));
                return false;
            }
        }
        
        dReal fstarttime = 0, fendtime = workspacetraj->GetTotalDuration();
        bool bPrevInCollision = true;
        list<Transform> listtransforms;
        for(dReal ftime = 0; ftime < workspacetraj->GetTotalDuration(); ftime += _parameters->_fStepLength) {
            workspacetraj->SampleTrajectory(ftime,pt);
            listtransforms.push_back(pt.trans);
            if( _manip->CheckEndEffectorCollision(pt.trans,_report) ) {
                if( _parameters->_bIgnoreFirstCollision && bPrevInCollision ) {
                    continue;
                }
                if( !bPrevInCollision ) {
                    if( ftime >= _parameters->_fMinimumCompleteTime ) {
                        fendtime = ftime;
                        break;
                    }
                }
                return false;
            }
            else {
                if( bPrevInCollision ) {
                    fstarttime = ftime;
                }
                bPrevInCollision = false;
            }
        }
        
        if( bPrevInCollision ) {
            // only the last point is valid
            fstarttime = workspacetraj->GetTotalDuration();
        }
        listtransforms.push_back(tlasttrans);
        
        // disable all child links since we've already checked their collision
        vector<KinBody::LinkPtr> vlinks;
        _manip->GetChildLinks(vlinks);
        FOREACH(it,vlinks) {
            (*it)->Enable(false);
        }
        
        if( !poutputtraj ) {
            poutputtraj = RaveCreateTrajectory(GetEnv(),"");
        }
        
        _mjacobian.resize(boost::extents[0][0]);
        _vprevsolution.resize(0);
        poutputtraj->Reset(_parameters->GetDOF());
        Transform tbaseinv = _manip->GetBase()->GetTransform().inverse();
        if( (int)_parameters->vinitialconfig.size() == _parameters->GetDOF() ) {
            _vprevsolution = _parameters->vinitialconfig;
            poutputtraj->AddPoint(Trajectory::TPOINT(_parameters->vinitialconfig,0));
            _parameters->_setstatefn(_parameters->vinitialconfig);
            _manip->CalculateJacobian(_mjacobian);
            _transprev = tbaseinv * _manip->GetEndEffectorTransform();
        }

        SetCustomFilterScope filter(_manip->GetIkSolver(),boost::bind(&WorkspaceTrajectoryTracker::_ValidateSolution,this,_1,_2,_3));
        vector<dReal> vsolution;
        if( !_parameters->_bGreedySearch ) {
            RAVELOG_ERROR("WorkspaceTrajectoryTracker::PlanPath - do not support non-greedy search\n");
        }

        list<Transform>::iterator ittrans = listtransforms.begin();
        bPrevInCollision = true;
        for(dReal ftime = 0; ftime < fendtime; ftime += _parameters->_fStepLength, ++ittrans) {
            int filteroptions = (ftime >= fstarttime) ? IKFO_CheckEnvCollisions : 0;
            if( !_manip->FindIKSolution(*ittrans,vsolution,filteroptions) ) {
                if( ftime < fstarttime ) {
                    return false; // a solution really doesn't exist
                }
                if( _parameters->_bIgnoreFirstCollision && bPrevInCollision ) {
                    if( !_manip->FindIKSolution(*ittrans,vsolution,0) ) {
                        return false;
                    }
                }
                else {
                    if( !bPrevInCollision ) {
                        if( ftime >= _parameters->_fMinimumCompleteTime ) {
                            fendtime = ftime;
                            break;
                        }
                    }
                    return false;
                }
            }
            else {
                bPrevInCollision = false;
            }

            poutputtraj->AddPoint(Trajectory::TPOINT(vsolution,0));
            _parameters->_setstatefn(vsolution);
            _manip->CalculateJacobian(_mjacobian);
            _transprev = tbaseinv * *ittrans;
        }

        RAVELOG_DEBUG(str(boost::format("plan success, path=%d points in %fs\n")%poutputtraj->GetPoints().size()%((0.001f*(float)(timeGetTime()-basetime)))));
        return true;
    }

    virtual PlannerParametersConstPtr GetParameters() const { return _parameters; }

protected:
    IkFilterReturn _ValidateSolution(std::vector<dReal>& vsolution, RobotBase::ManipulatorPtr pmanip, const IkParameterization& ikp)
    {
        if( !!_parameters->_constraintfn ) {
            _vtempsolution = vsolution;
            if( !_parameters->_constraintfn(_vprevsolution.size() > 0 ? _vprevsolution : vsolution, _vtempsolution,0) ) {
                return IKFR_Reject;
            }
            // check if solution was changed
            for(size_t j = 0; j < _vtempsolution.size(); ++j) {
                if( RaveFabs(_vtempsolution[j] - vsolution[j]) > 2*g_fEpsilon ) {
                    RAVELOG_WARN("solution changed by constraint function\n");
                    return IKFR_Reject;
                }
            }
        }
        
        // check if continuous with previous solution using the jacobian
        if( _mjacobian.num_elements() > 0 ) {
            Vector expecteddeltatrans = ikp.GetTransform().trans - _transprev.trans;
            Vector jdeltatrans;
            for(size_t j = 0; j < vsolution.size(); ++j) {
                dReal d = vsolution[j]-_vprevsolution.at(j);
                jdeltatrans.x += _mjacobian[0][j]*d;
                jdeltatrans.y += _mjacobian[1][j]*d;
                jdeltatrans.z += _mjacobian[2][j]*d;
            }
            dReal cangle = expecteddeltatrans.dot3(jdeltatrans);
            if( cangle < 0 || cangle*cangle  < _fMaxCosDeviationAngle*_fMaxCosDeviationAngle*expecteddeltatrans.lengthsqr3()*jdeltatrans.lengthsqr3() ) {
                return IKFR_Reject;
            }
        }

        return IKFR_Success;
    }

    RobotBasePtr _robot;
    RobotBase::ManipulatorPtr _manip;
    CollisionReportPtr _report;
    boost::shared_ptr<WorkspaceTrajectoryParameters> _parameters;
    dReal _fMaxCosDeviationAngle;

    // planning state
    boost::multi_array<dReal,2> _mjacobian;
    Transform _transprev;
    vector<dReal> _vprevsolution, _vtempsolution;
};

#endif