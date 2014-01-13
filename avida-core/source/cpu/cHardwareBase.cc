/*
 *  cHardwareBase.cc
 *  Avida
 *
 *  Called "hardware_base.cc" prior to 11/17/05.
 *  Copyright 1999-2011 Michigan State University. All rights reserved.
 *  Copyright 1999-2003 California Institute of Technology.
 *
 *
 *  This file is part of Avida.
 *
 *  Avida is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 *  Avida is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License along with Avida.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "cHardwareBase.h"

#include "avida/core/Feedback.h"
#include "avida/core/UniverseDriver.h"

#include "cCPUTestInfo.h"
#include "cEnvironment.h"
#include "cHardwareManager.h"
#include "cHardwareStatusPrinter.h"
#include "cOrganism.h"
#include "cPhenotype.h"
#include "cPopulation.h"
#include "cPopulationCell.h"
#include "cStats.h"
#include "cTestCPU.h"
#include "cWorld.h"

using namespace Avida;


cHardwareBase::cHardwareBase(cWorld* world, cOrganism* in_organism, cInstSet* inst_set)
: m_world(world), m_organism(in_organism), m_inst_set(inst_set), m_tracer(NULL)
, m_has_costs(inst_set->HasCosts()), m_has_ft_costs(inst_set->HasFTCosts())
, m_has_res_costs(m_inst_set->HasResCosts())
, m_has_post_costs(inst_set->HasPostCosts()), m_has_bonus_costs(inst_set->HasBonusCosts())
{
	m_has_any_costs = (m_has_costs | m_has_ft_costs | m_has_res_costs | m_has_post_costs);
  m_implicit_repro_active = (m_world->GetConfig().IMPLICIT_REPRO_TIME.Get() ||
                             m_world->GetConfig().IMPLICIT_REPRO_CPU_CYCLES.Get() ||
                             m_world->GetConfig().IMPLICIT_REPRO_BONUS.Get() ||
                             m_world->GetConfig().IMPLICIT_REPRO_END.Get());
	
  assert(m_organism != NULL);
}


void cHardwareBase::Reset(cAvidaContext& ctx)
{
  m_organism->HardwareReset(ctx);
  ResizeCostArrays(m_world->GetConfig().MAX_CPU_THREADS.Get());
  
  const int num_inst_cost = m_inst_set->GetSize();
  
  if (m_has_ft_costs) {
    m_inst_ft_cost.Resize(num_inst_cost);
    for (int i = 0; i < num_inst_cost; i++) m_inst_ft_cost[i] = m_inst_set->GetFTCost(Instruction(i));
  }
  
  if (m_has_res_costs) {
    m_inst_res_cost.Resize(num_inst_cost);
    for (int i = 0; i < num_inst_cost; i++) m_inst_res_cost[i] = m_inst_set->GetResCost(Instruction(i));
  }
  
  if (m_has_costs) {
    m_thread_inst_cost.Resize(num_inst_cost);
    for (int i = 0; i < num_inst_cost; i++) m_thread_inst_cost[i] = m_inst_set->GetCost(Instruction(i));
  }

  if (m_has_post_costs) {
    m_thread_inst_post_cost.Resize(num_inst_cost);
    for (int i = 0; i < num_inst_cost; i++) m_thread_inst_post_cost[i] = m_inst_set->GetPostCost(Instruction(i));
  }

  if (m_has_bonus_costs) {
    m_inst_bonus_cost.Resize(num_inst_cost);
    for (int i = 0; i < num_inst_cost; i++) m_inst_bonus_cost[i] = m_inst_set->GetBonusCost(Instruction(i));
  }

  internalReset();
}

void cHardwareBase::ResizeCostArrays(int new_size)
{
  m_active_thread_costs.Resize(new_size);
  m_active_thread_costs.SetAll(0);
  m_active_thread_post_costs.Resize(new_size);
  m_active_thread_post_costs.SetAll(0);
}

int cHardwareBase::calcExecutedSize(const int parent_size)
{
  int executed_size = 0;
  const InstMemSpace& memory = GetMemory();
  for (int i = 0; i < parent_size; i++) {
    if (memory.FlagExecuted(i)) executed_size++;
  }  
  return executed_size;
}

bool cHardwareBase::Divide_CheckViable(cAvidaContext& ctx, const int parent_size, const int child_size, bool using_repro)
{
  // Make sure the organism is okay with dividing now...
  // Moved to end of function @LZ

  // Make sure that neither parent nor child will be below the minimum size.
  const Genome& genome = m_organism->GetGenome();
  ConstInstructionSequencePtr seq_p;
  ConstGeneticRepresentationPtr rep_p = genome.Representation();
  seq_p.DynamicCastFrom(rep_p);
  const InstructionSequence& seq = *seq_p;
  
  const int genome_size = seq.GetSize();

  
  const int juv_age = m_world->GetConfig().JUV_PERIOD.Get();
  const int parent_age = m_organism->GetPhenotype().GetTimeUsed();
  if (parent_age < juv_age) return false;
  
  const int min_age = m_world->GetConfig().MIN_CYCLES.Get();
  if (parent_age < min_age) return false;

  // Make sure that neither parent nor child will be below the minimum size.  
  const double size_range = m_world->GetConfig().OFFSPRING_SIZE_RANGE.Get();
  const int min_size = Apto::Max(MIN_GENOME_LENGTH, static_cast<int>(genome_size / size_range));
  const int max_size = Apto::Min(MAX_GENOME_LENGTH, static_cast<int>(genome_size * size_range));
  
  if (child_size < min_size || child_size > max_size) return false; // (divide fails)
  if (parent_size < min_size || parent_size > max_size) return false; // (divide fails)
  
  // Absolute minimum and maximum child/parent size limits -- @JEB
  const int max_genome_size = m_world->GetConfig().MAX_GENOME_SIZE.Get();
  const int min_genome_size = m_world->GetConfig().MIN_GENOME_SIZE.Get();
  if ( (min_genome_size && (child_size < min_genome_size)) || (max_genome_size && (child_size > max_genome_size)) ) return false; // (divide fails)
  
  if ( (min_genome_size && (parent_size < min_genome_size)) || (max_genome_size && (parent_size > max_genome_size)) ) return false; // (divide fails)
  
  // Count the number of lines executed in the parent, and make sure the
  // specified fraction has been reached.
  
  const int executed_size = calcExecutedSize(parent_size);
  const int min_exe_lines = static_cast<int>(parent_size * m_world->GetConfig().MIN_EXE_LINES.Get());
  if (executed_size < min_exe_lines) return false; // (divide fails)
  
  // Repro organisms mark their entire genomes as copied
  int copied_size = parent_size;
  if (!using_repro) {
    // Normal organisms check to see how much was copied
    copied_size = calcCopiedSize(parent_size, child_size); // Fails for REPRO organisms
    const int min_copied = static_cast<int>(child_size * m_world->GetConfig().MIN_COPIED_LINES.Get());
    
    if (copied_size < min_copied) return false; // (divide fails)
  }

  if (m_organism->Divide_CheckViable(ctx) == false) 
  {
    if (m_world->GetConfig().DIVIDE_FAILURE_RESETS.Get())
    {
      internalResetOnFailedDivide();
    }
    return false; // (divide fails)
  }

  
  // Save the information we collected here...
  cPhenotype& phenotype = m_organism->GetPhenotype();
  phenotype.SetLinesExecuted(executed_size);
  phenotype.SetLinesCopied(copied_size);
  
  // Determine the fitness of this organism as compared to its parent...
  if (m_world->GetTestSterilize() && !phenotype.IsInjected()) {
    const int merit_base = phenotype.CalcSizeMerit();
    const double cur_fitness = merit_base * phenotype.GetCurBonus() / phenotype.GetTimeUsed();
    const double fitness_ratio = cur_fitness / phenotype.GetLastFitness();
    const Apto::Array<int>& childtasks = phenotype.GetCurTaskCount();
    const Apto::Array<int>& parenttasks = phenotype.GetLastTaskCount();
    
    bool sterilize = false;
    
    if (fitness_ratio < 1.0) {
      if (ctx.GetRandom().P(m_organism->GetSterilizeNeg())) sterilize = true;
    } else if (fitness_ratio == 1.0) {
      if (ctx.GetRandom().P(m_organism->GetSterilizeNeut())) sterilize = true;
    } else {
      if (ctx.GetRandom().P(m_organism->GetSterilizePos())) sterilize = true;
    }
    
    // for sterilize task loss *SLG
    if (ctx.GetRandom().P(m_organism->GetSterilizeTaskLoss()))
    {
      bool del = false;
      bool added = false;
      for (int i=0; i<childtasks.GetSize(); i++)
      {
        if (childtasks[i] > parenttasks[i]) {
          added = true;
          break;
        }
        else if (childtasks[i] < parenttasks[i])
          del = true;
      }
      sterilize = (del & !added);
    }
    
    if (sterilize) {
      // Don't let this organism have this or any more children!
      phenotype.IsFertile() = false;
      return false;
    }    
  }
  
  return true; // (divide succeeds!)
}


/*
 Return the number of mutations that occur on divide.  AWC 06/29/06
 Limit the number of mutations that occur to be less than or equal to maxmut (defaults to INT_MAX)
 */
int cHardwareBase::Divide_DoMutations(cAvidaContext& ctx, double mut_multiplier, const int maxmut)
{
  int max_genome_size = m_world->GetConfig().MAX_GENOME_SIZE.Get();
  int min_genome_size = m_world->GetConfig().MIN_GENOME_SIZE.Get();
  if (!max_genome_size || max_genome_size > MAX_GENOME_LENGTH) max_genome_size = MAX_GENOME_LENGTH;
  if (!min_genome_size || min_genome_size < MIN_GENOME_LENGTH) min_genome_size = MIN_GENOME_LENGTH;
  
  int totalMutations = 0;
  Genome& offspring_gen = m_organism->OffspringGenome();
  InstructionSequencePtr offspring_seq_p;
  GeneticRepresentationPtr offspring_rep_p = offspring_gen.Representation();
  offspring_seq_p.DynamicCastFrom(offspring_rep_p);
  InstructionSequence& offspring_genome = *offspring_seq_p;
  
  m_organism->GetPhenotype().SetDivType(mut_multiplier);
  
  // All slip and translocation mutations should happen first, so that there is a chance
  // of getting a point mutation within one copy in the same divide.
  
  // Divide Slip Mutations - NOT COUNTED.
  if (m_organism->TestDivideSlip(ctx)) doSlipMutation(ctx, offspring_genome);
  
  // Poisson Slip Mutations - NOT COUNTED
  unsigned int num_poisson_slip = m_organism->NumDividePoissonSlip(ctx);
  for (unsigned int i = 0; i < num_poisson_slip; i++) { doSlipMutation(ctx, offspring_genome);  }
  
  // Slip Mutations (per site) - NOT COUNTED
  if (m_organism->GetDivSlipProb() > 0) {
    int num_mut = ctx.GetRandom().GetRandBinomial(offspring_genome.GetSize(), 
                                                  m_organism->GetDivSlipProb() / mut_multiplier);
    for (int i = 0; i < num_mut; i++) doSlipMutation(ctx, offspring_genome);
  }

  
  // Divide Translocation Mutations - NOT COUNTED.
  if (m_organism->TestDivideTrans(ctx)) doTransMutation(ctx, offspring_genome);
  
  // Poisson Translocation Mutations - NOT COUNTED
  unsigned int num_poisson_trans = m_organism->NumDividePoissonTrans(ctx);
  for (unsigned int i = 0; i < num_poisson_trans; i++) { doTransMutation(ctx, offspring_genome);  }
  
  // Translocation Mutations (per site) - NOT COUNTED
  if (m_organism->GetDivTransProb() > 0) {
    int num_mut = ctx.GetRandom().GetRandBinomial(offspring_genome.GetSize(),
                                                  m_organism->GetDivTransProb() / mut_multiplier);
    for (int i = 0; i < num_mut; i++) doTransMutation(ctx, offspring_genome);
  }

  
  // Divide Mutations
  if (m_organism->TestDivideMut(ctx) && totalMutations < maxmut) {
    const unsigned int mut_line = ctx.GetRandom().GetUInt(offspring_genome.GetSize());
    offspring_genome[mut_line] = m_inst_set->GetRandomInst(ctx);
    totalMutations++;
  }
  
  
  // Poisson Divide Mutations
  unsigned int num_poisson_mut = m_organism->NumDividePoissonMut(ctx);
  for (unsigned int i=0; i<num_poisson_mut; i++)
  {
    if (totalMutations >= maxmut) break;
    const unsigned int mut_line = ctx.GetRandom().GetUInt(offspring_genome.GetSize());
    offspring_genome[mut_line] = m_inst_set->GetRandomInst(ctx);
    totalMutations++;
  }
  
  
  // Divide Insertions
  if (m_organism->TestDivideIns(ctx) && offspring_genome.GetSize() < max_genome_size && totalMutations < maxmut) {
    const unsigned int mut_line = ctx.GetRandom().GetUInt(offspring_genome.GetSize() + 1);
    offspring_genome.Insert(mut_line, m_inst_set->GetRandomInst(ctx));
    totalMutations++;
  }
  
  
  // Poisson Divide Insertions
  unsigned int num_poisson_ins = m_organism->NumDividePoissonIns(ctx);
  for (unsigned int i=0; i<num_poisson_ins; i++)
  {
    if (offspring_genome.GetSize() >= max_genome_size) break;
    if (totalMutations >= maxmut) break;
    const unsigned int mut_line = ctx.GetRandom().GetUInt(offspring_genome.GetSize() + 1);
    offspring_genome.Insert(mut_line, m_inst_set->GetRandomInst(ctx));
    totalMutations++;
  }
  
  
  // Divide Deletions
  if (m_organism->TestDivideDel(ctx) && offspring_genome.GetSize() > min_genome_size && totalMutations < maxmut) {
    const unsigned int mut_line = ctx.GetRandom().GetUInt(offspring_genome.GetSize());
    offspring_genome.Remove(mut_line);
    totalMutations++;
  }
  
  
  // Poisson Divide Deletions
  unsigned int num_poisson_del = m_organism->NumDividePoissonDel(ctx);
  for (unsigned int i=0; i<num_poisson_del; i++)
  {
    if (offspring_genome.GetSize() <= min_genome_size) break;
    if (totalMutations >= maxmut) break;
    const unsigned int mut_line = ctx.GetRandom().GetUInt(offspring_genome.GetSize());
    offspring_genome.Remove(mut_line);
    totalMutations++;
  }
  
  
  // Divide Uniform Mutations
  if (m_organism->TestDivideUniform(ctx) && totalMutations < maxmut) {
    if (doUniformMutation(ctx, offspring_genome)) totalMutations++;
  }
  
  
  // Divide Mutations (per site)
  if (m_organism->GetDivMutProb() > 0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(offspring_genome.GetSize(), 
                                                  m_organism->GetDivMutProb() / mut_multiplier);
    // If we have lines to mutate...
    if (num_mut > 0 && totalMutations < maxmut) {
      for (int i = 0; i < num_mut && totalMutations < maxmut; i++) {
        int site = ctx.GetRandom().GetUInt(offspring_genome.GetSize());
        offspring_genome[site] = m_inst_set->GetRandomInst(ctx);
        totalMutations++;
      }
    }
  }
  
  // Insert Mutations (per site)
  if (m_organism->GetDivInsProb() > 0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(offspring_genome.GetSize(), m_organism->GetDivInsProb());
    
    // If would make creature too big, insert up to max_genome_size
    if (num_mut + offspring_genome.GetSize() > max_genome_size) {
      num_mut = max_genome_size - offspring_genome.GetSize();
    }
    
    // If we have lines to insert...
    if (num_mut > 0) {
      // Build a sorted list of the sites where mutations occured
      Apto::Array<int> mut_sites(num_mut);
      for (int i = 0; i < num_mut; i++) mut_sites[i] = ctx.GetRandom().GetUInt(offspring_genome.GetSize() + 1);
      Apto::QSort(mut_sites);
      
      // Actually do the mutations (in reverse sort order)
      for (int i = mut_sites.GetSize() - 1; i >= 0; i--) {
        offspring_genome.Insert(mut_sites[i], m_inst_set->GetRandomInst(ctx));
      }
      
      totalMutations += num_mut;
    }
  }
  
  
  // Delete Mutations (per site)
  if (m_organism->GetDivDelProb() > 0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(offspring_genome.GetSize(), m_organism->GetDivDelProb());
    
    // If would make creature too small, delete down to min_genome_size
    if (offspring_genome.GetSize() - num_mut < min_genome_size) {
      num_mut = offspring_genome.GetSize() - min_genome_size;
    }
    
    // If we have lines to delete...
    for (int i = 0; i < num_mut; i++) {
      int site = ctx.GetRandom().GetUInt(offspring_genome.GetSize());
      offspring_genome.Remove(site);
    }
    
    totalMutations += num_mut;
  }
  
  
  
  // Uniform Mutations (per site)
  if (m_organism->GetDivUniformProb() > 0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(offspring_genome.GetSize(), 
                                                  m_organism->GetDivUniformProb() / mut_multiplier);
    
    // If we have lines to mutate...
    if (num_mut > 0 && totalMutations < maxmut) {
      for (int i = 0; i < num_mut && totalMutations < maxmut; i++) {
        if (doUniformMutation(ctx, offspring_genome)) totalMutations++;
      }
    }
  }
    
  
  InstMemSpace& memory = GetMemory();

  // Parent Substitution Mutations (per site)
  if (m_organism->GetParentMutProb() > 0.0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(memory.GetSize(), m_organism->GetParentMutProb());
    
    // If we have lines to mutate...
    if (num_mut > 0) {
      for (int i = 0; i < num_mut && totalMutations < maxmut; i++) {
        int site = ctx.GetRandom().GetUInt(memory.GetSize());
        memory[site] = m_inst_set->GetRandomInst(ctx);
        totalMutations++;
      }
    }
  }
  
  // Parent Insert Mutations (per site)
  if (m_organism->GetParentInsProb() > 0.0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(memory.GetSize(), m_organism->GetParentInsProb());
    
    // If would make creature too big, insert up to max_genome_size
    if (num_mut + memory.GetSize() > max_genome_size) {
      num_mut = max_genome_size - memory.GetSize();
    }
    
    // If we have lines to insert...
    if (num_mut > 0) {
      // Build a sorted list of the sites where mutations occured
      Apto::Array<int> mut_sites(num_mut);
      for (int i = 0; i < num_mut; i++) mut_sites[i] = ctx.GetRandom().GetUInt(memory.GetSize() + 1);
      Apto::QSort(mut_sites);
      
      // Actually do the mutations (in reverse sort order)
      for (int i = mut_sites.GetSize() - 1; i >= 0; i--) {
        memory.Insert(mut_sites[i], m_inst_set->GetRandomInst(ctx));
      }
      
      totalMutations += num_mut;
    }
  }
  
  
  // Parent Deletion Mutations (per site)
  if (m_organism->GetParentDelProb() > 0 && totalMutations < maxmut) {
    int num_mut = ctx.GetRandom().GetRandBinomial(memory.GetSize(), m_organism->GetParentDelProb());
    
    // If would make creature too small, delete down to min_genome_size
    if (memory.GetSize() - num_mut < min_genome_size) {
      num_mut = memory.GetSize() - min_genome_size;
    }
    
    // If we have lines to delete...
    for (int i = 0; i < num_mut; i++) {
      int site = ctx.GetRandom().GetUInt(memory.GetSize());
      memory.Remove(site);
    }
    
    totalMutations += num_mut;
  }

  
  return totalMutations;
}


bool cHardwareBase::doUniformMutation(cAvidaContext& ctx, InstructionSequence& genome)
{
  
  int mut = ctx.GetRandom().GetUInt((m_inst_set->GetSize() * 2) + 1);
  
  if (mut < m_inst_set->GetSize()) { // point
    int site = ctx.GetRandom().GetUInt(genome.GetSize());
    genome[site] = Instruction(mut);
  } else if (mut == m_inst_set->GetSize()) { // delete
    int min_genome_size = m_world->GetConfig().MIN_GENOME_SIZE.Get();
    if (!min_genome_size || min_genome_size < MIN_GENOME_LENGTH) min_genome_size = MIN_GENOME_LENGTH;
    if (genome.GetSize() == min_genome_size) return false;
    int site = ctx.GetRandom().GetUInt(genome.GetSize());
    genome.Remove(site);
  } else { // insert
    int max_genome_size = m_world->GetConfig().MAX_GENOME_SIZE.Get();
    if (!max_genome_size || max_genome_size > MAX_GENOME_LENGTH) max_genome_size = MAX_GENOME_LENGTH;
    if (genome.GetSize() == max_genome_size) return false;
    int site = ctx.GetRandom().GetUInt(genome.GetSize() + 1);
    genome.Insert(site, Instruction(mut - m_inst_set->GetSize() - 1));
  }
  
  return true;
}

void cHardwareBase::doUniformCopyMutation(cAvidaContext& ctx, cHeadCPU& head)
{
  int mut = ctx.GetRandom().GetUInt((m_inst_set->GetSize() * 2) + 1);
  //Anya added code for Head to Head kazi experiment
  bool in_List = false;
  if (mut < m_inst_set->GetSize()) head.SetInst(Instruction(mut));
  else if (mut == m_inst_set->GetSize()) head.RemoveInst();
  else head.InsertInst(Instruction(mut - m_inst_set->GetSize() - 1));
}



// Slip Mutations
// As if the read head jumped from one random position of the offspring
// to another random position and continued reading to the end.
// This can cause large deletions or tandem duplications.
// Unlucky organisms might exceed the allowed length (randomly) if these mutations occur.
void cHardwareBase::doSlipMutation(cAvidaContext& ctx, InstructionSequence& genome, int from)
{
  InstructionSequence genome_copy = InstructionSequence(genome);
  
  // All combinations except beginning to past end allowed
  if (from < 0) from = ctx.GetRandom().GetInt(genome_copy.GetSize() + 1);
  int to = (from == 0) ? ctx.GetRandom().GetInt(genome_copy.GetSize()) : ctx.GetRandom().GetInt(genome_copy.GetSize() + 1);
  
  // Resize child genome
  int insertion_length = (from - to);
  genome.Resize(genome.GetSize() + insertion_length);
  
  // Fill insertion
  if (insertion_length > 0) {
    Apto::Array<bool> copied_so_far(insertion_length);
    copied_so_far.SetAll(false);
    for (int i = 0; i < insertion_length; i++) {
      switch (m_world->GetConfig().SLIP_FILL_MODE.Get()) {
          //Duplication
        case 0:
          genome[from + i] = genome_copy[to + i];
          break;
          
          //Empty (nop-X)
        case 1:
          genome[from + i] = m_inst_set->GetInst("nop-X");
          break;
          
          //Random
        case 2:
          genome[from + i] = m_inst_set->GetRandomInst(ctx);
          break;
          
          //Scrambled order
        case 3:
        {
          int copy_index = ctx.GetRandom().GetInt(insertion_length - i);
          int test = 0;
          int passed = copy_index;
          while (passed >= 0) {
            if (copied_so_far[test]) {
              copy_index++; 
            } else { //this one hasn't been chosen, so we count it.
              passed--;
            }
            test++;
          }
          genome[from + i] = genome[to + copy_index];
          copied_so_far[copy_index] = true;
        }
          break;
          
          //Empty (nop-C)
        case 4:
          genome[from + i] = m_inst_set->GetInst("nop-C");
          break;
          
        default:
          ctx.Driver().Feedback().Error("Unknown SLIP_FILL_MODE");
          ctx.Driver().Abort(Avida::INVALID_CONFIG);
      }
    }
  }
  
  // Deletion / remaining genome
  if (insertion_length < 0) insertion_length = 0;
  for (int i = insertion_length; i < genome_copy.GetSize() - to; i++) genome[from + i] = genome_copy[to + i];
  
  if (m_world->GetVerbosity() >= VERBOSE_DETAILS) {
    cout << "SLIP MUTATION from " << from << " to " << to << endl;
    cout << "Parent: " << genome_copy.AsString()   << endl;
    cout << "Offspring: " << genome.AsString() << endl;
  }
}



// Translocation Mutations
// Similar to slip mutations, described above.  However, insertion location is also chosen randomly.
void cHardwareBase::doTransMutation(cAvidaContext& ctx, InstructionSequence& genome, int from)
{
  InstructionSequence genome_copy(genome);
  
  // All combinations except beginning to past end allowed
  if (from < 0) from = ctx.GetRandom().GetInt(genome_copy.GetSize() + 1);
  int to = (from == 0) ? ctx.GetRandom().GetInt(genome_copy.GetSize()) : ctx.GetRandom().GetInt(genome_copy.GetSize() + 1);
  
  // Resize child genome
  int insertion_length = (from - to);
  genome.Resize(genome.GetSize() + insertion_length);
  
  // Select insertion location
  int ins_loc = ctx.GetRandom().GetInt(genome_copy.GetSize() + 1);
  
  // Fill insertion
  if (insertion_length > 0) {
    switch (m_world->GetConfig().TRANS_FILL_MODE.Get()) {
        //Duplication
      case 0:
        for (int i = 0; i < insertion_length; i++) genome[ins_loc + i] = genome_copy[to + i];
        break;
        
        //Scrambled order
      case 1:
      {
        Apto::Array<bool> copied_so_far(insertion_length);
        copied_so_far.SetAll(false);
        for (int i = 0; i < insertion_length; i++) {
          int copy_index = ctx.GetRandom().GetInt(insertion_length - i);
          int test = 0;
          int passed = copy_index;
          while (passed >= 0) {
            if (copied_so_far[test]) {
              copy_index++;
            } else { //this one hasn't been chosen, so we count it.
              passed--;
            }
            test++;
          }
          genome[ins_loc + i] = genome[to + copy_index];
          copied_so_far[copy_index] = true;
        }
      }
        break;
        
      default:
        ctx.Driver().Feedback().Error("Unknown TRANS_FILL_MODE");
        ctx.Driver().Abort(Avida::INVALID_CONFIG);
    }
  }
  
  if (insertion_length < 0) {
    // Deletion
    for (int i = ins_loc; i < genome.GetSize(); i++) genome[i] = genome_copy[i - insertion_length];
  } else {
    // Copy remaining genome
    for (int i = ins_loc; i < genome_copy.GetSize(); i++) genome[i + insertion_length] = genome_copy[i];
  }
}



/*
 Return the number of mutations that occur on divide.  AWC 06/29/06
 Limit the number of mutations that occur to be less than or equat to maxmut (defaults to INT_MAX)
 */
unsigned cHardwareBase::Divide_DoExactMutations(cAvidaContext& ctx, double mut_multiplier, const int pointmut)
{
  int maxmut = pointmut;
  int totalMutations = 0;
  Genome& offspring_genome = m_organism->OffspringGenome();
  InstructionSequencePtr offspring_seq_p;
  GeneticRepresentationPtr offspring_rep_p = offspring_genome.Representation();
  offspring_seq_p.DynamicCastFrom(offspring_rep_p);
  InstructionSequence& child_genome = *offspring_seq_p;
  
  m_organism->GetPhenotype().SetDivType(mut_multiplier);
  
  // Divide Mutations
  if (totalMutations < maxmut) {
    const unsigned int mut_line = ctx.GetRandom().GetUInt(child_genome.GetSize());
    child_genome[mut_line] = m_inst_set->GetRandomInst(ctx);
    totalMutations++;
  }
  
  // Divide Mutations (per site)
  if (m_organism->GetDivMutProb() > 0 && totalMutations < maxmut) {
    int num_mut = pointmut;
    // If we have lines to mutate...
    if (num_mut > 0 && totalMutations < maxmut) {
      for (int i = 0; i < num_mut && totalMutations < maxmut; i++) {
        int site = ctx.GetRandom().GetUInt(child_genome.GetSize());
        child_genome[site] = m_inst_set->GetRandomInst(ctx);
        totalMutations++;
        cerr << "Resampling here " << totalMutations << endl;
      }
    }
  }
  
  return totalMutations;
}


// test whether the offspring creature contains an advantageous mutation.
/*
 Return true iff only a reversion is performed -- returns false is sterilized regardless of whether or 
 not a reversion is performed.  AWC 06/29/06
 */
bool cHardwareBase::Divide_TestFitnessMeasures(cAvidaContext& ctx)
{
  cPhenotype & phenotype = m_organism->GetPhenotype();
  phenotype.CopyTrue() = ( m_organism->OffspringGenome() == m_organism->GetGenome() );
  phenotype.ChildFertile() = true;
	
  // Only continue if we're supposed to do a fitness test on divide...
  // This means you must add a check for your config option to cWorld::setup()
  // -- search for "m_test_on_div".
  if (m_organism->GetTestOnDivide() == false) return false;
	
  // If this was a perfect copy, then we don't need to worry about any other
  // tests...  Theoretically, we need to worry about the parent changing,
  // but as long as the child is always compared to the original genotype,
  // this won't be an issue.
  if (phenotype.CopyTrue() == true) return false;
	
  const double parent_fitness = m_organism->GetTestFitness(ctx);
  const Apto::Array<int>& parenttasks = phenotype.GetCurTaskCount();
  const double neut_min = parent_fitness * (1.0 - m_organism->GetNeutralMin());
  const double neut_max = parent_fitness * (1.0 + m_organism->GetNeutralMax());
  
  cTestCPU* testcpu = m_world->GetHardwareManager().CreateTestCPU(ctx);
  cCPUTestInfo test_info;
  test_info.UseRandomInputs();
  testcpu->TestGenome(ctx, test_info, m_organism->OffspringGenome());
  const double child_fitness = test_info.GetGenotypeFitness();
  delete testcpu;
  
  bool revert = false;
  bool sterilize = false;
  
  // If implicit mutations are turned off, make sure this won't spawn one.
  if (m_organism->GetSterilizeUnstable() == true) {
    if (test_info.GetMaxDepth() > 0) sterilize = true;
  }
  
  if (child_fitness == 0.0) {
    // Fatal mutation... test for reversion.
    if (ctx.GetRandom().P(m_organism->GetRevertFatal())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizeFatal())) sterilize = true;
  } else if (child_fitness < neut_min) {
    if (ctx.GetRandom().P(m_organism->GetRevertNeg())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizeNeg())) sterilize = true;
  } else if (child_fitness <= neut_max) {
    if (ctx.GetRandom().P(m_organism->GetRevertNeut())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizeNeut())) sterilize = true;
  } else {
    if (ctx.GetRandom().P(m_organism->GetRevertPos())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizePos())) sterilize = true;
  }
  
  // If task loss without gain is to be sterilized or reverted,
  // check for it and act appropriately.
  int RorS = 0;	// 0 = neither, 1 = revert, 2 = sterilize
  if (ctx.GetRandom().P(m_organism->GetRevertTaskLoss()))
    RorS = 1;
  else if (ctx.GetRandom().P(m_organism->GetSterilizeTaskLoss()))
    RorS = 2;
  // check if child has lost any tasks parent had AND not gained any new tasks
  if (RorS) {
    const Apto::Array<int>& childtasks = test_info.GetTestPhenotype().GetLastTaskCount();
    bool del = false;
    bool added = false;
    for (int i=0; i<childtasks.GetSize(); i++)
    {
      if (childtasks[i] > parenttasks[i]) {
        added = true;
        break;
      }
      else if (childtasks[i] < parenttasks[i])
        del = true;
    }
    if (RorS == 1) 
      revert = (del & !added);
    else 
      sterilize = (del & !added);
  }

  // If mutations granting EQU should be reverted, check for EQU
  // and flag for reversion.
  // The probabilistic check is placed under guard so as not to 
  // affect consistency by using a random number if the feature
  // is not used.
  if (m_organism->GetRevertEquals() != 0) {
    if (ctx.GetRandom().P(m_organism->GetRevertEquals())) {
      const Apto::Array<int>& child_tasks = test_info.GetTestPhenotype().GetLastTaskCount();
      if (child_tasks[child_tasks.GetSize() - 1] >= 1) {
        revert = true;
        m_world->GetStats().AddNewTaskCount(child_tasks.GetSize() - 1);
      }
    }
  }
  
  // Ideally, we won't have reversions and sterilizations turned on at the
  // same time, but if we do, give revert the priority.
  if (revert == true) {
    m_organism->OffspringGenome() = m_organism->GetGenome();
  }
  
  if (sterilize == true) {
    m_organism->GetPhenotype().ChildFertile() = false;
  }
  
  return (!sterilize) && revert;
}

// test whether the offspring creature contains an advantageous mutation.
/*
 Return true iff only a reversion is performed -- returns false is sterilized regardless of whether or 
 not a reversion is performed.  AWC 06/29/06
 */
bool cHardwareBase::Divide_TestFitnessMeasures1(cAvidaContext& ctx)
{
  cPhenotype & phenotype = m_organism->GetPhenotype();
  phenotype.CopyTrue() = (m_organism->OffspringGenome() == m_organism->GetGenome());
  phenotype.ChildFertile() = true;
	
  // Only continue if we're supposed to do a fitness test on divide...
  // This means you must add a check for your config option to cWorld::setup()
  // -- search for "m_test_on_div".
  if (m_organism->GetTestOnDivide() == false) return false;
	
  // If this was a perfect copy, then we don't need to worry about any other
  // tests...  Theoretically, we need to worry about the parent changing,
  // but as long as the child is always compared to the original genotype,
  // this won't be an issue.
  if (phenotype.CopyTrue() == true) return false;
	
  const double parent_fitness = m_organism->GetTestFitness(ctx);
  const Apto::Array<int>& parenttasks = phenotype.GetCurTaskCount();
  const double neut_min = parent_fitness * (1.0 - m_organism->GetNeutralMin());
  const double neut_max = parent_fitness * (1.0 + m_organism->GetNeutralMax());
  
  cTestCPU* testcpu = m_world->GetHardwareManager().CreateTestCPU(ctx);
  cCPUTestInfo test_info;
  test_info.UseRandomInputs();
  testcpu->TestGenome(ctx, test_info, m_organism->OffspringGenome());
  const double child_fitness = test_info.GetGenotypeFitness();
  delete testcpu;
  
  bool revert = false;
  bool sterilize = false;
  
  // If implicit mutations are turned off, make sure this won't spawn one.
  if (m_organism->GetSterilizeUnstable() > 0) {
    if (test_info.GetMaxDepth() > 0) sterilize = true;
  }
  
  if (m_organism->GetSterilizeUnstable() > 1 && !test_info.IsViable()) {
    sterilize = true;
  }
  
  if (child_fitness == 0.0) {
    // Fatal mutation... test for reversion.
    if (ctx.GetRandom().P(m_organism->GetRevertFatal())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizeFatal())) sterilize = true;
  } else if (child_fitness < neut_min) {
    if (ctx.GetRandom().P(m_organism->GetRevertNeg())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizeNeg())) sterilize = true;
  } else if (child_fitness <= neut_max) {
    if (ctx.GetRandom().P(m_organism->GetRevertNeut())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizeNeut())) sterilize = true;
  } else {
    if (ctx.GetRandom().P(m_organism->GetRevertPos())) revert = true;
    if (ctx.GetRandom().P(m_organism->GetSterilizePos())) sterilize = true;
  }
  
  int RorS = 0;	// 0 = neither, 1 = revert, 2 = sterilize
  if (ctx.GetRandom().P(m_organism->GetRevertTaskLoss()))
	  RorS = 1;
  else if (ctx.GetRandom().P(m_organism->GetSterilizeTaskLoss()))
	  RorS = 2;
  // check if child has lost any tasks parent had AND not gained any new tasks
  if (RorS) {
	  const Apto::Array<int>& childtasks = test_info.GetTestPhenotype().GetLastTaskCount();
	  bool del = false;
	  bool added = false;
	  for (int i=0; i<childtasks.GetSize(); i++)
	  {
		  if (childtasks[i] > parenttasks[i]) {
			  added = true;
			  break;
		  }
		  else if (childtasks[i] < parenttasks[i])
			  del = true;
	  }
	  if (RorS == 1) 
		  revert = (del & !added);
	  else 
		  sterilize = (del & !added);
  }
  
  // If mutations granting EQU should be reverted, check for EQU
  // and flag for reversion.
  // The probabilistic check is placed under guard so as not to 
  // affect consistency by using a random number if the feature
  // is not used.
  if (m_organism->GetRevertEquals() != 0) {
    if (ctx.GetRandom().P(m_organism->GetRevertEquals())) {
      const Apto::Array<int>& child_tasks = test_info.GetTestPhenotype().GetLastTaskCount();
      if (child_tasks[child_tasks.GetSize() - 1] >= 1) {
        revert = true;
        m_world->GetStats().AddNewTaskCount(child_tasks.GetSize() - 1);
      }
    }
  }
  
  // Ideally, we won't have reversions and sterilizations turned on at the
  // same time, but if we do, give revert the priority.
  if (revert == true) {
	  m_organism->OffspringGenome() = m_organism->GetGenome();
  }
  
  if (sterilize == true) {
	  m_organism->GetPhenotype().ChildFertile() = false;
  }
  
  return (!sterilize) && revert;
}

int cHardwareBase::PointMutate(cAvidaContext& ctx, double override_mut_rate)
{
  const int max_genome_size = m_world->GetConfig().MAX_GENOME_SIZE.Get();
  const int min_genome_size = m_world->GetConfig().MIN_GENOME_SIZE.Get();
  
  InstMemSpace& memory = GetMemory();
  int totalMutations = 0;
  
//  const int num_muts = ctx.GetRandom().GetRandBinomial(memory.GetSize(), mut_rate);
//  
//  for (int i = 0; i < num_muts; i++) {
//    const int pos = ctx.GetRandom().GetUInt(memory.GetSize());
//    memory[pos] = m_inst_set->GetRandomInst(ctx);
//    memory.SetFlagMutated(pos);
//    memory.SetFlagPointMut(pos);
//  }

  
  // Point Substitution Mutations (per site)
  if (m_organism->GetPointMutProb() > 0.0 || override_mut_rate > 0.0) {
    double mut_rate = (override_mut_rate > 0.0) ? override_mut_rate : m_organism->GetPointMutProb();
    int num_mut = ctx.GetRandom().GetRandBinomial(memory.GetSize(), mut_rate);
    
    // If we have lines to mutate...
    if (num_mut > 0) {
      for (int i = 0; i < num_mut; i++) {
        int site = ctx.GetRandom().GetUInt(memory.GetSize());
        memory[site] = m_inst_set->GetRandomInst(ctx);
        totalMutations++;
      }
    }
  }
  
  // Point Insert Mutations (per site)
  if (m_organism->GetPointInsProb() > 0.0) {
    int num_mut = ctx.GetRandom().GetRandBinomial(memory.GetSize(), m_organism->GetPointInsProb());
    
    // If would make creature too big, insert up to max_genome_size
    if (num_mut + memory.GetSize() > max_genome_size) {
      num_mut = max_genome_size - memory.GetSize();
    }
    
    // If we have lines to insert...
    if (num_mut > 0) {
      // Build a sorted list of the sites where mutations occured
      Apto::Array<int> mut_sites(num_mut);
      for (int i = 0; i < num_mut; i++) mut_sites[i] = ctx.GetRandom().GetUInt(memory.GetSize() + 1);
      Apto::QSort(mut_sites);
      
      // Actually do the mutations (in reverse sort order)
      for (int i = mut_sites.GetSize() - 1; i >= 0; i--) {
        memory.Insert(mut_sites[i], m_inst_set->GetRandomInst(ctx));
      }
      
      totalMutations += num_mut;
    }
  }
  
  
  // Point Deletion Mutations (per site)
  if (m_organism->GetPointDelProb() > 0) {
    int num_mut = ctx.GetRandom().GetRandBinomial(memory.GetSize(), m_organism->GetPointDelProb());
    
    // If would make creature too small, delete down to min_genome_size
    if (memory.GetSize() - num_mut < min_genome_size) {
      num_mut = memory.GetSize() - min_genome_size;
    }
    
    // If we have lines to delete...
    for (int i = 0; i < num_mut; i++) {
      int site = ctx.GetRandom().GetUInt(memory.GetSize());
      memory.Remove(site);
    }
    
    totalMutations += num_mut;
  }
  return totalMutations;
}




bool cHardwareBase::Inst_Nop(cAvidaContext&)          // Do Nothing.
{
  return true;
}


// @JEB Check implicit repro conditions -- meant to be called at the end of SingleProcess
void cHardwareBase::checkImplicitRepro(cAvidaContext& ctx, bool exec_last_inst)         
{  
  //Dividing a dead organism causes all kinds of problems
  if (m_organism->IsDead()) return;
  
  if( (m_world->GetConfig().IMPLICIT_REPRO_TIME.Get() && (m_organism->GetPhenotype().GetTimeUsed() >= m_world->GetConfig().IMPLICIT_REPRO_TIME.Get()))
     || (m_world->GetConfig().IMPLICIT_REPRO_CPU_CYCLES.Get() && (m_organism->GetPhenotype().GetCPUCyclesUsed() >= m_world->GetConfig().IMPLICIT_REPRO_CPU_CYCLES.Get()))
     || (m_world->GetConfig().IMPLICIT_REPRO_BONUS.Get() && (m_organism->GetPhenotype().GetCurBonus() >= m_world->GetConfig().IMPLICIT_REPRO_BONUS.Get()))
     || (m_world->GetConfig().IMPLICIT_REPRO_END.Get() && exec_last_inst))
  {
    Inst_Repro(ctx);
  }
}

//This must be overridden by the specific CPU to function properly
bool cHardwareBase::Inst_Repro(cAvidaContext&) 
{
  cout << "This hardware type does not have a =repro= instruction. IMPLICIT_REPRO conditions cannot be used!" << endl;
  exit(1);
  return false;
}



// This method will test to see if all costs have been paid associated
// with executing an instruction and only return true when that instruction
// should proceed.
bool cHardwareBase::SingleProcess_PayPreCosts(cAvidaContext& ctx, const Instruction& cur_inst, const int thread_id)
{ 
  
  // If first time cost hasn't been paid off...
  if (m_has_ft_costs && m_inst_ft_cost[cur_inst.GetOp()] > 0) {
    m_inst_ft_cost[cur_inst.GetOp()]--;       // dec cost
    return false;
  }
  
  // Check for res_costs and fail if org does not have enough resources to process instruction
  // As post-cost, res_cost will not be paid unless all pre-costs are paid and all restrictions inside of instruction pass
  if (m_has_res_costs || m_has_fem_res_costs) {
    
    double res_cost = m_inst_set->GetResCost(cur_inst); 
    double fem_res_cost = m_organism->GetPhenotype().GetMatingType() == MATING_TYPE_FEMALE ? m_inst_set->GetFemResCost(cur_inst) : 0; 
    double res_req = res_cost + fem_res_cost;

    const Apto::Array<double> res_count = m_organism->GetOrgInterface().GetResources(ctx);
    Apto::Array<double> res_change(res_count.GetSize());
    res_change.SetAll(0.0);
    
    const int resource = m_world->GetConfig().COLLECT_SPECIFIC_RESOURCE.Get();
    if (resource < 0) cout << "Instruction resource costs require use of COLLECT_SPECIFIC_RESOURCE and USE_RESOURCE_BINS" << '\n';
    assert(resource >= 0);
    
    double res_stored = m_organism->GetRBin(resource);
    // fail if org does not have enough resources to process instruction
    if (res_stored < res_req) return false;
  }

  if (m_has_bonus_costs) {
    if (m_organism->GetPhenotype().GetCurBonus() < m_inst_set->GetBonusCost(cur_inst)) return false;
  }

  //@CHC: If this organism is female, or a choosy female, we may need to impose additional costs for her to execute the instruction
  int per_use_cost = 0;
  if (m_has_costs) {
    per_use_cost = m_thread_inst_cost[cur_inst.GetOp()]; //*THIS IS THE LINE!!!!
  }
  bool add_female_costs = false;
  if (m_has_female_costs) {
    if (m_organism->GetPhenotype().GetMatingType() == MATING_TYPE_FEMALE) {
      if (m_inst_set->GetFemaleCost(cur_inst)) {
        add_female_costs = true;
        per_use_cost += m_inst_set->GetFemaleCost(cur_inst);
      }
    }
  } 
  bool add_choosy_female_costs = false;
  if (m_has_choosy_female_costs) {
    if ((m_organism->GetPhenotype().GetMatingType() == MATING_TYPE_FEMALE) & (m_organism->GetPhenotype().GetMatePreference() != MATE_PREFERENCE_RANDOM)) {
      if (m_inst_set->GetChoosyFemaleCost(cur_inst)) {
        add_choosy_female_costs = true;
        per_use_cost += m_inst_set->GetChoosyFemaleCost(cur_inst);
      }
    }
  }
  // Next, look at the per use costs
  if (m_has_costs | add_female_costs | add_choosy_female_costs | m_has_post_costs) {    
    // Current ACTIVE thread-specific execution cost being paid, decrement and return false 
    // if ACTIVE post cost already being paid (from previous executed instruction), pay this before doing anything else
    if (m_active_thread_post_costs[thread_id] > 1) {
      m_active_thread_post_costs[thread_id]--;
      return false;
    }
    if (m_active_thread_post_costs[thread_id] == 1) m_active_thread_post_costs[thread_id] = 0;
    
    if (m_active_thread_costs[thread_id] > 1) { 
      m_active_thread_costs[thread_id]--;
      return false;
    }
    
    // no already active thread-specific execution cost, but this instruction has a cost, setup the counter and return false 
    // if active PRE costs, pay these before executing instruction
    if (!m_active_thread_costs[thread_id] && per_use_cost > 1) { //used to be: m_thread_inst_cost[cur_inst.GetOp()] > 1) {
      m_active_thread_costs[thread_id] = per_use_cost - 1;
      return false;
    }     
    // If we fall to here, execution is allowed now...any pre-cost is paid 
    if (m_active_thread_costs[thread_id] == 1) m_active_thread_costs[thread_id] = 0;
  }
  
  return true;
}

bool cHardwareBase::IsPayingActiveCost(cAvidaContext& ctx, const int thread_id)
{
  if (m_active_thread_post_costs[thread_id] > 1) return true;
  return false;
}

void cHardwareBase::SingleProcess_PayPostResCosts(cAvidaContext& ctx, const Instruction& cur_inst)
{
  if (m_has_res_costs || m_has_fem_res_costs) {
    
    double res_cost = m_inst_set->GetResCost(cur_inst); 
    double fem_res_cost = m_organism->GetPhenotype().GetMatingType() == MATING_TYPE_FEMALE ? m_inst_set->GetFemResCost(cur_inst) : 0; 
    double res_req = res_cost + fem_res_cost;
    
    const Apto::Array<double> res_count = m_organism->GetOrgInterface().GetResources(ctx);
    Apto::Array<double> res_change(res_count.GetSize());
    res_change.SetAll(0.0);
    
    const int resource = m_world->GetConfig().COLLECT_SPECIFIC_RESOURCE.Get();
    if (resource < 0) cout << "Instruction resource costs require use of COLLECT_SPECIFIC_RESOURCE and USE_RESOURCE_BINS" << '\n';
    assert(resource >= 0);
    
    // subtract res used from current bin by adding negative value
    double cost = res_req * -1.0;
    m_organism->AddToRBin(resource, cost); 
  }
  if (m_has_bonus_costs) m_organism->GetPhenotype().SetCurBonus(m_organism->GetPhenotype().GetCurBonus() - m_inst_set->GetBonusCost(cur_inst));

  return;
}

void cHardwareBase::SingleProcess_SetPostCPUCosts(cAvidaContext&, const Instruction& cur_inst, const int thread_id)
{
  if (m_has_post_costs) {
    int per_use_post_cost = m_thread_inst_post_cost[cur_inst.GetOp()];
    // for post-cost, setup the new counter after allowing initial execution to proceed...this will cause the next instruction to pause before execution
    if (!m_active_thread_post_costs[thread_id] && per_use_post_cost > 1) { 
      m_active_thread_post_costs[thread_id] = per_use_post_cost;
    }      
  }
  return;
}

