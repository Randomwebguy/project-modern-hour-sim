// SPDX-License-Identifier: GPL-3.0-or-later
#include "VehicleWear.h"
#include <iostream>
#include <stdexcept>
using namespace GenZH::Modern;
int main(){try{
 unsigned checks=0;auto require=[&](bool p){++checks;if(!p)throw std::runtime_error("vehicle appearance contract");};
 VehicleWearState first,second;
 auto fresh=first.Advance(1,100,0,0,4,10,1,true);require(fresh.age[0]==0&&fresh.age[1]==0&&fresh.age[2]==0&&fresh.age[3]==1);
 first.Bind(101,fresh);require(GetVehicleWear(101).age[0]==0);
 auto moved=first.Advance(1,400,500,0,4,10,.7f,true);require(moved.age[0]>.3f&&moved.age[1]>.2f&&moved.age[2]==0);
 auto duplicate=first.Advance(1,400,500,0,4,10,.7f,true);require(duplicate.age==moved.age);
 auto newNeighbor=second.Advance(2,400,500,0,4,10,1,true);second.Bind(202,newNeighbor);first.Bind(101,moved);require(GetVehicleWear(202).age[0]==0&&GetVehicleWear(101).age[0]>.3f);
 auto repaired=first.Advance(1,430,500,0,4,10,1,true);require(repaired.age[0]>=moved.age[0]);
 auto late=first.Advance(1,36100,500,0,4,10,1,true);require(late.age[2]>0&&late.age[2]<late.age[0]);
 first.Bind(303,late);require(GetVehicleWear(101).age[3]==0&&GetVehicleWear(303).age[0]>0);
 auto construction=first.Advance(1,36130,500,0,4,10,1,false);require(construction.age[3]==0);
 auto rebuilt=first.Advance(1,36160,500,0,4,10,1,true);require(rebuilt.age[0]==0);
 auto reset=second.Advance(2,10,0,0,0,10,1,true);require(reset.age[0]==0);
 auto teleport=second.Advance(2,11,100000,0,0,10,1,true);require(teleport.age[1]<.001f);
 first.Unbind();second.Unbind();require(VehicleWearBindings().empty());
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"fresh_clean\":true,\"per_vehicle_isolation\":true,\"pause_same_frame\":true,\"repair_retains_wear\":true,\"late_corrosion\":true,\"LOD_rebind_preserves_age\":true,\"teleport_filtered\":true,\"game_started\":false}";
 return 0;
 }catch(const std::exception&e){std::cerr<<e.what();return 1;}}
