/*
  Copyright (C) 2011,2012,2013,2014 The ESPResSo project
  
  This file is part of ESPResSo.
  
  ESPResSo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  ESPResSo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>. 
*/

#include "collision.hpp"
#include "cells.hpp"
#include "communication.hpp"
#include "errorhandling.hpp"
#include "grid.hpp"
#include "domain_decomposition.hpp"
#include "gb.hpp"

using namespace std;


#ifdef COLLISION_DETECTION_DEBUG
#define TRACE(a) a
#else
#define TRACE(a)
#endif

#ifdef COLLISION_DETECTION

/// Data type holding the info about a single collision
typedef struct {
  int pp1; // 1st particle id
  int pp2; // 2nd particle id
  double point_of_collision[3]; 
} collision_struct;

// During force calculation, colliding particles are recorded in thequeue
// The queue is processed after force calculation, when it is save to add
// particles
static collision_struct *collision_queue = 0;
static collision_struct *gathered_queue = 0;

// Number of collisions recoreded in the queue
static int number_of_collisions, total_collisions;

/// Parameters for collision detection
Collision_parameters collision_params = { 0, };

int collision_detection_set_params(int mode, double d, int bond_centers, int bond_vs, int t, int d2, int tg, int tv, int ta, int bond_three_particles, int angle_resolution, double triangle_size)
{
  // The collision modes involving virutal sites also require the creation of a bond between the colliding 
  // particles, hence, we turn that on.
  if ((mode & COLLISION_MODE_VS) || (mode & COLLISION_MODE_GLUE_TO_SURF) || (COLLISION_MODE_TRIANGLE_BINDING))
    mode |= COLLISION_MODE_BOND;

  if (mode & COLLISION_MODE_BIND_THREE_PARTICLES)
    mode |= COLLISION_MODE_BOND;

  // If we don't have virtual sites, virtual site binding isn't possible.
#ifndef VIRTUAL_SITES_RELATIVE
  if ((mode & COLLISION_MODE_VS) || (mode & COLLISION_MODE_GLUE_TO_SURF) || (mode & COLLISION_MODE_TRIANGLE_BINDING))
    return 1;
#endif

  // For vs based methods, Binding so far only works on a single cpu
  //if ((mode & COLLISION_MODE_VS) || (mode & COLLISION_MODE_GLUE_TO_SURF))
  if ((mode & COLLISION_MODE_VS) || (mode & COLLISION_MODE_GLUE_TO_SURF) || (mode & COLLISION_MODE_TRIANGLE_BINDING    ))
    if (n_nodes != 1)
      return 2;

  // Check if bonded ia exist
  if ((mode & COLLISION_MODE_BOND) &&
      (bond_centers >= n_bonded_ia))
    return 3;
  if ((mode & COLLISION_MODE_VS) &&
      (bond_vs >= n_bonded_ia))
    return 3;
  
  // If the bond type to bind particle centers is not a pair bond...
  // Check that the bonds have the right number of partners
  if ((mode & COLLISION_MODE_BOND) &&
      (bonded_ia_params[bond_centers].num != 1))
    return 4;
  
  // The bond between the virtual sites can be pair or triple
  if ((mode & COLLISION_MODE_VS) && !(bonded_ia_params[bond_vs].num == 1 ||
				      bonded_ia_params[bond_vs].num == 2))
    return 5;
  
  if (mode & COLLISION_MODE_BIND_THREE_PARTICLES) {
    if (bond_three_particles + angle_resolution >= n_bonded_ia)
      return 6;
    
    for (int i = bond_three_particles; i <= bond_three_particles + angle_resolution; i++) {
      if (bonded_ia_params[i].num != 2)
        return 7;
    }
  }

  // Set params
  collision_params.mode=mode;
  collision_params.bond_centers=bond_centers;
  collision_params.bond_vs=bond_vs;
  collision_params.distance=d;
  collision_params.vs_particle_type=t;
  collision_params.dist_glued_part_to_vs =d2;
  collision_params.part_type_to_be_glued =tg;
  collision_params.part_type_to_attach_vs_to =tv;
  collision_params.part_type_after_glueing =ta;
  collision_params.bond_three_particles =bond_three_particles;
  collision_params.three_particle_angle_resolution =angle_resolution;
  collision_params.triangle_size = triangle_size;

  if (mode & COLLISION_MODE_VS || COLLISION_MODE_TRIANGLE_BINDING || COLLISION_MODE_TRIANGLE_BINDING)
    make_particle_type_exist(t);
  
  
  if (mode & COLLISION_MODE_GLUE_TO_SURF)
  {
    make_particle_type_exist(t);
    make_particle_type_exist(tg);
    make_particle_type_exist(tv);
    make_particle_type_exist(ta);
  }
  
  mpi_bcast_collision_params();
  recalc_forces = 1;
  return 0;
}


//* Allocate memory for the collision queue /
void prepare_collision_queue()
{
 TRACE(printf("%d: Prepare_collision_queue()\n",this_node));
  number_of_collisions=0;
}


bool bond_exists(Particle* p, Particle* partner, int bond_type)
{
  // First check the bonds of p1
  if (p->bl.e) {
    int i = 0;
    while(i < p->bl.n) {
      int size = bonded_ia_params[p->bl.e[i]].num;
      
      if (p->bl.e[i] == bond_type &&
          p->bl.e[i + 1] == partner->p.identity) {
        // There's a bond, already. Nothing to do for these particles
        return true;
      }
      i += size + 1;
    }
  }
  return false;
}

//can be used for analyzing agglomerates
void queue_collision(int part1,int part2, double* point_of_collision) {

    //Get memory for the new entry in the collision queue
    number_of_collisions++;
    if (number_of_collisions==1)
      collision_queue = (collision_struct *) malloc(number_of_collisions*sizeof(collision_struct));
    else
      collision_queue = (collision_struct *) realloc(collision_queue,number_of_collisions*sizeof(collision_struct));
    // Save the collision      
    collision_queue[number_of_collisions-1].pp1 = part1;
    collision_queue[number_of_collisions-1].pp2 = part2;
    // The C library function void *memcpy(void *str1, const void *str2, size_t n) copies n characters from memory area str2 to memory area str1.
    // the structure: void memcpy(destination, source, size_t)
    memcpy(collision_queue[number_of_collisions-1].point_of_collision,point_of_collision, 3*sizeof(double));
    
    //TRACE(printf("%d: Added to queue: Particles %d and %d at %lf %lf %lf\n",this_node,part1,part2,point_of_collision[0],point_of_collision[1],point_of_collision[2]));
    //printf("%d: Added to queue: Particles %d and %d collided at %f %f %f\n",this_node,part1,part2,&point_of_collision[0],&point_of_collision[1],&point_of_collision[2]);
    //printf("%d: Added to queue: Particles %d and %d at %lf %lf %lf\n",this_node,part1,part2,&point_of_collision[0],&point_of_collision[1],&point_of_collision[2]);
}


// Detect a collision between the given particles.
// Add it to the queue in case virtual sites should be added at the point of collision
void detect_collision(Particle* p1, Particle* p2)
{
  // The check, whether collision detection is actually turned on is performed in forces.hpp

  int part1, part2, size;
  int counts[n_nodes];

  //TRACE(printf("%d: consider particles %d and %d\n", this_node, p1->p.identity, p2->p.identity));

  double vec21[3];
  // Obtain distance between particles
  double dist_betw_part = sqrt(distance2vec(p1->r.p, p2->r.p, vec21));
  TRACE(printf("%d: Distance between particles %lf %lf %lf, Scalar: %f\n",this_node,vec21[0],vec21[1],vec21[2], dist_betw_part));
 //it might be an error here, try with distance  
 //if (dist_betw_part > max_cut_nonbonded)
  if (dist_betw_part > collision_params.distance) 
    return;


// Calculate here Gay-Berne nonbonded energy (code in gb.hpp)
    

  double gb_en;
  IA_parameters *ia_params = get_ia_param(p1->p.type, p2->p.type);
  gb_en = gb_pair_energy(p1, p2, ia_params,
                       vec21, dist_betw_part);

  if (gb_en >= -0.001 and gb_en <= 0.001)
    return;

  //TRACE(printf("%d: particles %d and %d within bonding distance %lf\n", this_node, p1->p.identity, p2->p.identity, dist_betw_part));
  // If we are in the glue to surface mode, check that the particles
  // are of the right type
  if (collision_params.mode & COLLISION_MODE_GLUE_TO_SURF) {
    if (! (
       ((p1->p.type==collision_params.part_type_to_be_glued)
       && (p2->p.type ==collision_params.part_type_to_attach_vs_to))
      ||
       ((p2->p.type==collision_params.part_type_to_be_glued)
       && (p1->p.type ==collision_params.part_type_to_attach_vs_to)))
     ) { 
       return;
     }
   }

  part1 = p1->p.identity;
  part2 = p2->p.identity;
      
  // Retrieving the particles from local_particles is necessary, because the particle might be a
  // ghost, and those can't store bonding info.
  p1 = local_particles[part1];
  p2 = local_particles[part2];

#ifdef VIRTUAL_SITES_RELATIVE
  // Ignore virtual particles
  if ((p1->p.isVirtual) || (p2->p.isVirtual))
    return;
#endif

  if (p1==p2)
    return;

  // Check, if there's already a bond between the particles

//MILENA-23.5.2016.
  if (bond_exists(p1,p2, collision_params.bond_centers))
    return;
  
  if (bond_exists(p2,p1, collision_params.bond_centers))
    return;


  TRACE(printf("%d: no previous bond, binding\n", this_node));
  printf("%d: no previous bond, binding\n", this_node);

  /* If we're still here, there is no previous bond between the particles,
     we have a new collision */

  if (collision_params.mode & COLLISION_MODE_BOND) {

    // do not create bond between ghost particles
    if (p1->l.ghost && p2->l.ghost) {
       TRACE(printf("Both particles %d and %d are ghost particles", p1->p.identity, p2->p.identity));
       return;
    }

  
  double new_position[3];
    /* If we also create virtual sites or bind three particles, or throw an exception, we add the collision
       to the queue to process later */
    // Point of collision
    double c;
    // If not in the glue_to_surface-mode, the point of collision
    // is in the middle of the vector connecting the particle
    // centers
// ASSUME WE DON'T NEED MULTIPLE OCCURING OF THIS  
     
    if (! (collision_params.mode & COLLISION_MODE_GLUE_TO_SURF))
      c=0.5;
    else
    {
      // Find out, which is the particle to be glued.
      // Swap particle, if need be
    if ((p1->p.type==collision_params.part_type_to_be_glued)
       && (p2->p.type ==collision_params.part_type_to_attach_vs_to))
       { 
	   c = collision_params.dist_glued_part_to_vs/dist_betw_part;
       }
       else if ((p2->p.type==collision_params.part_type_to_be_glued)
          && (p1->p.type ==collision_params.part_type_to_attach_vs_to))
       { 
         // we swap the particle ids so that the virtual site is always attached to p2
         int tmp=part1;
         part1=part2;
         part2=tmp;
	 // We need the negative sign for the prefactor, as we di
	 // we did not flip the vec21 when swapping particles
	 c = -collision_params.dist_glued_part_to_vs/dist_betw_part;
       }
       else
       {
        printf("Something is wrong %s %d\n",__FILE__,":"+__LINE__);
       }
     }
     //printf("We are here, meaning particles %d and %d have collided\n", p1->p.identity, p2->p.identity);
     for (int i=0;i<3;i++) {
       new_position[i] = p1->r.p[i] - vec21[i] * c;
    }
    queue_collision(part1,part2,new_position);
  }
}



// Considers three particles for three_particle_binding and performs
// the binding if the criteria are met //
void coldet_do_three_particle_bond(Particle* p, Particle* p1, Particle* p2)
{
  double vec21[3];
  // If p1 and p2 are not closer or equal to the cutoff distance, skip
  // p1:
  get_mi_vector(vec21,p->r.p,p1->r.p);
  if (sqrt(sqrlen(vec21)) > collision_params.distance)
    return;
  // p2:
  get_mi_vector(vec21,p->r.p,p2->r.p);
  if (sqrt(sqrlen(vec21)) > collision_params.distance)
    return;

  //TRACE(printf("%d: checking three particle bond %d %d %d\n", this_node, p1->p.identity, p->p.identity, p2->p.identity))
  // Check, if there already is a three-particle bond centered on p 
  // with p1 and p2 as partners. If so, skip this triplet.
  // Note that the bond partners can appear in any order.
 
  // Iterate over existing bonds of p

  if (p->bl.e) {
    int b = 0;
    while (b < p->bl.n) {
      int size = bonded_ia_params[p->bl.e[b]].num;

      //TRACE(printf("%d:--1-- checking bond of type %d and length %d of particle %d\n", this_node, p->bl.e[b], bonded_ia_params[p->bl.e[b]].num, p->p.identity));
 
      if (size==2) {
        // Check if the bond type is within the range used by the collision detection,
        if ((p->bl.e[b] >= collision_params.bond_three_particles) & (p->bl.e[b] <=collision_params.bond_three_particles + collision_params.three_particle_angle_resolution)) {
          // check, if p1 and p2 are the bond partners, (in any order)
          // if yes, skip triplet
          if (
              ((p->bl.e[b+1]==p1->p.identity) && (p->bl.e[b+2] ==p2->p.identity))
              ||
              ((p->bl.e[b+1]==p2->p.identity) && (p->bl.e[b+2] ==p1->p.identity))
              )
            return;
        } // if bond type 
      } // if size==2
      
      // Go to next bond
      b += size + 1;
    } // bond loop
  } // if bond list defined

  //TRACE(printf("%d: proceeding to install three particle bond %d %d %d\n", this_node, p1->p.identity, p->p.identity, p2->p.identity));

  // If we are still here, we need to create angular bond
  // First, find the angle between the particle p, p1 and p2
  double cosine=0.0;
  
  double vec1[3],vec2[3];
  /* vector from p to p1 */
  get_mi_vector(vec1, p->r.p, p1->r.p);
  // Normalize
  double dist2 = sqrlen(vec1);
  double d1i = 1.0 / sqrt(dist2);
  for(int j=0;j<3;j++) vec1[j] *= d1i;
  
  /* vector from p to p2 */
  get_mi_vector(vec2, p->r.p, p2->r.p);
  // normalize
  dist2 = sqrlen(vec2);
  double d2i = 1.0 / sqrt(dist2);
  for(int j=0;j<3;j++) vec2[j] *= d2i;
  
  /* scalar product of vec1 and vec2 */
  cosine = scalar(vec1, vec2);
  
  // Handle case where cosine is nearly 1 or nearly -1
  if ( cosine >  TINY_COS_VALUE)  
    cosine = TINY_COS_VALUE;
  if ( cosine < -TINY_COS_VALUE)  
    cosine = -TINY_COS_VALUE;
  
  // Bond angle
  double phi =  acos(cosine);
  
  // We find the bond id by dividing the range from 0 to pi in 
  // three_particle_angle_resolution steps and by adding the id
  // of the bond for zero degrees.
  int bond_id =floor(phi/M_PI * (collision_params.three_particle_angle_resolution-1) +0.5) + collision_params.bond_three_particles;
  
  // Create the bond
  
  // First, fill bond data structure
  int bondT[3];
  bondT[0] = bond_id;
  bondT[1] = p1->p.identity;
  bondT[2] = p2->p.identity;
  local_change_bond(p->p.identity, bondT, 0);
}

// If activated, throws an exception for each collision which can be
// parsed by the script interface
void handle_exception_throwing_for_single_collision(int i)
{
    if (collision_params.mode & (COLLISION_MODE_EXCEPTION)) {

      int id1, id2;
      if (collision_queue[i].pp1 > collision_queue[i].pp2) {
	id1 = collision_queue[i].pp2;
	id2 = collision_queue[i].pp1;
      }
      else {
	id1 = collision_queue[i].pp1;
	id2 = collision_queue[i].pp2;
      }
      ostringstream msg;
      msg << "collision between particles " << id1 << " and " <<id2;
      runtimeError(msg);
    }
}

#ifdef VIRTUAL_SITES_RELATIVE
void place_vs_and_relate_to_particle(double* pos, int relate_to)
{
          //printf("Previous max seen particle is %d\n",max_seen_particle);	  
	  place_particle(max_seen_particle+1,pos);
          
	  vs_relate_to(max_seen_particle,relate_to);
                    
	  (local_particles[max_seen_particle])->p.isVirtual=1;
	  #ifdef ROTATION_PER_PARTICLE
	    (local_particles[relate_to])->p.rotation=14;
	  #endif
	  (local_particles[max_seen_particle])->p.type=collision_params.vs_particle_type;

}


void bind_at_poc_create_bond_between_vs(int i)
{
   int bondG[3];

   switch (bonded_ia_params[collision_params.bond_vs].num) {
   case 1: {
     // Create bond between the virtual particles
     bondG[0] = collision_params.bond_vs;
     bondG[1] = max_seen_particle-1;
     local_change_bond(max_seen_particle, bondG, 0);
     break;
   }
   case 2: {
     // Create 1st bond between the virtual particles
     bondG[0] = collision_params.bond_vs;
     bondG[1] = collision_queue[i].pp1;
     bondG[2] = collision_queue[i].pp2;
     local_change_bond(max_seen_particle,   bondG, 0);
     local_change_bond(max_seen_particle-1, bondG, 0);
     //HACK: Zero length bond of id 3 between virtual sites
     bondG[0] = 3;
     bondG[1] = max_seen_particle-1;
     local_change_bond(max_seen_particle,   bondG, 0);
     break;
   }
  }
}


/** gives a random vector perpendicular to the given vector through its given middle point*/
inline void get_mi_random_vector(double *perpendicular_vector, double *given_vector, double *middle_point)
{
 double z_rand          = d_random()*2-1;
 double alfa_rand       = d_random()*M_PI;
 double random_point[3] = {sqrt(1-z_rand*z_rand)*cos(alfa_rand),sqrt(1-z_rand*z_rand)*sin(alfa_rand),z_rand};
 vector_product(given_vector, random_point, perpendicular_vector);
 return;
}

/** the function calling get_mi_random_vector and calculating the three corners where virtual particles should be inserted */
void triangle_binding_calc_corners (Particle* p1, Particle* p2, double (&corners)[3][3]) 
{
  double connecting_vector[3];
  get_mi_vector(connecting_vector, p1->r.p, p2->r.p);
  double abs_connecting_vector=sqrt(sqrlen(connecting_vector));
  
  double c_m[3]; //middle point at the connecting_vector
  for (int i=0;i<3;i++)
    c_m[i] = p1->r.p[i]-0.5*connecting_vector[i]; 
  
  double orthogonal_vector[3];
  double director_sqr=-1;

  while ((director_sqr<=0) || scalar(orthogonal_vector,connecting_vector)/abs_connecting_vector>=0.99) {
    get_mi_random_vector(orthogonal_vector, connecting_vector, c_m);
    director_sqr=sqrlen(orthogonal_vector);
  }
  double abs_director1=sqrt(director_sqr);
  double director1[3],director2[3],director3[3];
  // normalize vector
  for (int i=0; i<3; i++) { 
    director1[i]=orthogonal_vector[i]*.5/abs_director1;
	};
 //   printf("the normalized random vector is %f %f %f\n",director1);
    double abs_director_2, abs_director_3;
    // get additional two vectors: director2 and director3 from rotation of director1 for 120 and 240 deg
    vec_rotate(connecting_vector, 2.*M_PI/3., director1, director2);
    vec_rotate(connecting_vector, 2.*M_PI/3., director2, director3);
    double corner_1[3], corner_2[3], corner_3[3]; //corners of the triangle
    for (int b=0; b<3; b++){ 
      corners[0][b]=c_m[b]+director1[b];
      corners[1][b]=c_m[b]+director2[b];
      corners[2][b]=c_m[b]+director3[b];
    }; 
//printf("TRIANGLE_BINDING corners are %f %f %f %f %f %f %f %f %f\n",corner_1[0],corner_1[1],corner_1[2],corner_2[0],corner_2[1],corner_2[2],corner_3[0],corner_3[1],corner_3[2]); 
    
 return;  
}
/***** */

void ellipsoid_collision(int i) 
{
    Particle* p1 = local_particles[collision_queue[i].pp1];
    Particle* p2 = local_particles[collision_queue[i].pp2];
    //printf("particle 1 and particle 2 are %d %d\n", p1->p.identity, p2->p.identity);
    //printf("local_particles 1 and 2 are %d %d\n", *local_particles[collision_queue[i].pp1],*local_particles[collision_queue[i].pp2]);
    // array definition, calculated from triangle_binding function  
    double (three_corners[3][3]);
    triangle_binding_calc_corners (p1, p2, (three_corners));
    
    for (int corner=0;corner<3;corner++) {
      //printf("Corner %d, id1 %d, id2 %d/n", corner, collision_queue[i].pp1,collision_queue[i].pp2);
      place_vs_and_relate_to_particle(three_corners[corner],collision_queue[i].pp1);
      place_vs_and_relate_to_particle(three_corners[corner],collision_queue[i].pp2);
      //bind_at_poc_create_bond_between_vs(i);
      int bondTriangle[3];      
      bondTriangle[0] = 3;
      bondTriangle[1] = max_seen_particle-1;
      local_change_bond(max_seen_particle,   bondTriangle, 0);
    }
       
          //printf("Particle from handle collisions inserted at %f %f %f and related to %d and %d\n", first_corner[0], first_corner[1],first_corner[2], p1->p.identity, p2->p.identity);    
   
    return;

}


/**** */
void glue_to_surface_bind_vs_to_pp1(int i)
{
	 int bondG[3];
         // Create bond between the virtual particles
         bondG[0] = collision_params.bond_vs;
         bondG[1] = max_seen_particle;
         local_change_bond(collision_queue[i].pp1, bondG, 0);
	 local_particles[collision_queue[i].pp1]->p.type=collision_params.part_type_after_glueing;
}

#endif

void gather_collision_queue(int* counts)
{
    int displacements[n_nodes];                   // offsets into collisions
  
    // Initialize number of collisions gathered from all processors
    for (int a=0;a<n_nodes;a++)
        counts[a]=0;
    
    // Total number of collisions
    MPI_Allreduce(&number_of_collisions, &total_collisions, 1, MPI_INT, MPI_SUM, comm_cart);
    
    if (total_collisions==0)
      return;

    // Gather number of collisions
    MPI_Allgather(&number_of_collisions, 1, MPI_INT, counts, 1, MPI_INT, comm_cart);

    // initialize displacement information for all nodes
    displacements[0]=0;
  
    // Find where to place collision information for each processor
    int byte_counts[n_nodes];
    for (int k=1; k<n_nodes; k++)
      displacements[k]=displacements[k-1]+(counts[k-1])*sizeof(collision_struct);
    
    for (int k=0; k<n_nodes; k++)
      byte_counts[k]=counts[k]*sizeof(collision_struct);
    
    TRACE(printf("counts [%d] = %d and number of collisions = %d and diplacements = %d and total collisions = %d\n", this_node, counts[this_node], number_of_collisions, displacements[this_node], total_collisions));
    
    // Allocate mem for the new collision info
    gathered_queue = (collision_struct *) malloc(total_collisions * sizeof(collision_struct));

    // Gather collision informtion from all nodes and send it to all nodes
    MPI_Allgatherv(collision_queue, byte_counts[this_node], MPI_BYTE, gathered_queue, byte_counts, displacements, MPI_BYTE, comm_cart);

    return;
}


// this looks in all local particles for a particle close to those in a 
// 2-particle collision. If it finds them, it performs three particle binding
void three_particle_binding_full_search()
{
  Cell *cell;
  Particle *p, *p1, *p2;
  // first iterate over cells, get one of the cells and find how many particles in this cell
  for (int c=0; c<local_cells.n; c++) {
      cell=local_cells.cell[c];
      // iterate over particles in the cell
      for (int a=0; a<cell->n; a++) {
          p=&cell->part[a];
          // for all p:
          for (int ij=0; ij<total_collisions; ij++) {
              p1=local_particles[gathered_queue[ij].pp1];
              p2=local_particles[gathered_queue[ij].pp2];
  
  		   // Check, whether p is equal to one of the particles in the
  		   // collision. If so, skip
  		   if ((p->p.identity ==p1->p.identity) || ( p->p.identity == p2->p.identity)) {
  		     continue;
  		   }
  
             // The following checks, 
  		 // if the particle p is closer that the cutoff from p1 and/or p2.
  		 // If yes, three particle bonds are created on all particles
  		 // which have two other particles within the cutoff distance,
  		 // unless such a bond already exists
  		 
  		 // We need all cyclical permutations, here 
  		 // (bond is placed on 1st particle, order of bond partners
  		 // does not matter, so we don't neet non-cyclic permutations):
             coldet_do_three_particle_bond(p,p1,p2);
             coldet_do_three_particle_bond(p1,p,p2);
             coldet_do_three_particle_bond(p2,p,p1);
  
         }
     }
 }

//TRACE(printf("particle %d and %d colided with", p1->p.identity, p2->p.identity));
}


// Goes through the collision queue and for each pair in it
// looks for a third particle by using the domain decomposition
// cell system. If found, it performs three particle binding
void three_particle_binding_domain_decomposition()
{
  // We have domain decomposition
    
  // Indices of the cells in which the colliding particles reside
  int cellIdx[2][3];
    
  // Iterate over collision queue

  for (int id=0;id<total_collisions;id++) {

      // Get first cell Idx
      if ((local_particles[gathered_queue[id].pp1] != NULL) && (local_particles[gathered_queue[id].pp2] != NULL)) {

        Particle* p1=local_particles[gathered_queue[id].pp1];
        Particle* p2=local_particles[gathered_queue[id].pp2];
        dd_position_to_cell_indices(p1->r.p,cellIdx[0]);
        dd_position_to_cell_indices(p2->r.p,cellIdx[1]);

        // Iterate over the cells + their neighbors
        // if p1 and p2 are in the same cell, we don't need to consider it 2x
        int lim=1;

        if ((cellIdx[0][0]==cellIdx[1][0]) && (cellIdx[0][1]==cellIdx[1][1]) && (cellIdx[0][2]==cellIdx[1][2]))
          lim=0; // Only consider the 1st cell

        for (int j=0;j<=lim;j++) {

            // Iterate the cell with indices cellIdx[j][] and all its neighbors.
            // code taken from dd_init_cell_interactions()
            for(int p=cellIdx[j][0]-1; p<=cellIdx[j][0]+1; p++)	
               for(int q=cellIdx[j][1]-1; q<=cellIdx[j][1]+1; q++)
	                for(int r=cellIdx[j][2]-1; r<=cellIdx[j][2]+1; r++) {   
	                   int ind2 = get_linear_index(p,q,r,dd.ghost_cell_grid);
	                   Cell* cell=cells+ind2;
 
	                   // Iterate over particles in this cell
                     for(int a=0; a<cell->n; a++) {
                        Particle* P=&cell->part[a];
                        // for all p:
  	                      // Check, whether p is equal to one of the particles in the
  	                      // collision. If so, skip
  	                      if ((P->p.identity ==p1->p.identity) || (P->p.identity == p2->p.identity)) {
                          //TRACE(printf("same particle\n"));
  		                continue;
  	                      }

                        // The following checks, 
                        // if the particle p is closer that the cutoff from p1 and/or p2.
                        // If yes, three particle bonds are created on all particles
  	                      // which have two other particles within the cutoff distance,
                        // unless such a bond already exists

  	                      // We need all cyclical permutations, here 
                        // (bond is placed on 1st particle, order of bond partners
  	                      // does not matter, so we don't need non-cyclic permutations):

                        if (P->l.ghost) {
                          //TRACE(printf("%d: center particle is ghost: %d\n", this_node, P->p.identity));
                          continue;
                        }
                        //TRACE(printf("%d: LOOP: %d Handling collision of particles FIRST CONFIGURATION %d %d %d\n", this_node, id, p1->p.identity, P->p.identity, p2->p.identity));
                        coldet_do_three_particle_bond(P,p1,p2);

                        if (p1->l.ghost) {
                          //TRACE(printf("%d: center particle is ghost: %d\n", this_node, p1->p.identity));
                          continue;
                        }

                        coldet_do_three_particle_bond(p1,P,p2);

                        if (p2->l.ghost) {
                          //TRACE(printf("%d: center particle is ghost: %d\n", this_node, p2->p.identity));
                          continue;
                        }

  	                      coldet_do_three_particle_bond(p2,P,p1);

                     } // loop over particles in this cell

	                } // Loop over cell

        } // Loop over particles if they are in different cells
    
      } // If local particles exist

  } // Loop over total collisions
}


// Handle the collisions stored in the queue
void handle_collisions ()
{
  //TRACE(printf("node %d: handle_collisions: number of collisions in queue %d\n",this_node,number_of_collisions));  
  if (collision_params.mode & COLLISION_MODE_EXCEPTION)
    for (int i=0;i<number_of_collisions;i++) {
      handle_exception_throwing_for_single_collision(i);
    }  


  if (collision_params.mode & COLLISION_MODE_BOND) 
  {
    for (int i=0;i<number_of_collisions;i++) {
      // put the bond to the physical particle; at least one partner always is
      int primary =collision_queue[i].pp1;
      int secondary = collision_queue[i].pp2;
      if (local_particles[collision_queue[i].pp1]->l.ghost) {
        primary = collision_queue[i].pp2;
        secondary = collision_queue[i].pp1;
        TRACE(printf("%d: particle-%d is ghost", this_node, collision_queue[i].pp1));
      }
      int bondG[2];
      bondG[0]=collision_params.bond_centers;
      bondG[1]=secondary;
      local_change_bond(primary, bondG, 0);
      TRACE(printf("%d: Adding bond %d->%d\n",this_node, primary,secondary));
    }
  }


#ifdef VIRTUAL_SITES_RELATIVE
  // If one of the collision modes is active which places virtual sites, we go over the queue to handle them
  if ((collision_params.mode & COLLISION_MODE_VS) || (collision_params.mode & COLLISION_MODE_GLUE_TO_SURF) || (collision_params.mode & COLLISION_MODE_TRIANGLE_BINDING)) 
    {
    for (int i=0;i<number_of_collisions;i++) {
	// Create virtual site(s)
        if (collision_params.mode & COLLISION_MODE_TRIANGLE_BINDING)
        {
          //printf("COUNTER i=number of collisions is %d\n", number_of_collisions); 
          ellipsoid_collision(i); 
          continue;
        } 

	// Virtual site related to first particle in the collision
	if (collision_params.mode & COLLISION_MODE_VS)
	{
	 place_vs_and_relate_to_particle(collision_queue[i].point_of_collision,collision_queue[i].pp1);
	}
	// The virtual site related to p2 is needed independently on which of the vs-related modes is active
        place_vs_and_relate_to_particle(collision_queue[i].point_of_collision,collision_queue[i].pp2);
  
	// If we are in the two vs mode, we need a bond between the virtual sites
	if (collision_params.mode & COLLISION_MODE_VS)
	{
          bind_at_poc_create_bond_between_vs(i);
        }
	
	// If we are in the "glue to surface mode", we need a bond between p1 and the vs
	if (collision_params.mode & COLLISION_MODE_GLUE_TO_SURF)
	{
           glue_to_surface_bind_vs_to_pp1(i);
        }
       //return;
       } // Loop over all collisions in the queue
     } // are we in one of the vs_based methods


  // three-particle-binding part

#endif //defined VIRTUAL_SITES_RELATIVE
 


  if (collision_params.mode & (COLLISION_MODE_BIND_THREE_PARTICLES))
  {
  int counts[n_nodes];
  gather_collision_queue(counts);

    if (counts[this_node]>0) {

      // If we don't have domain decomposition, we need to do a full sweep over all
      // particles in the system. (slow)
      if (cell_structure.type!=CELL_STRUCTURE_DOMDEC) {
        three_particle_binding_full_search();
       } // if cell structure != domain decomposition
    else
    {
      three_particle_binding_domain_decomposition();
    } // If we have doamin decomposition

    }   // if number of collisions of this node > 0
       
       if (total_collisions>0)
         free(gathered_queue);
       total_collisions = 0;
  } // if TPB

  // If a collision method is active which places particles, resorting might be needed
  //TRACE(printf("%d: Resort particles is %d\n",this_node,resort_particles));
  if (collision_params.mode & (COLLISION_MODE_VS || COLLISION_MODE_GLUE_TO_SURF || COLLISION_MODE_TRIANGLE_BINDING))
  {
    // NOTE!! this has to be changed to total_collisions, once parallelization
    // is implemented

    if (number_of_collisions >0)
    {
      announce_resort_particles();
    }
  }
  
  // Reset the collision queue
  if (number_of_collisions>0)
    free(collision_queue);
  
  number_of_collisions = 0;


}

#endif
