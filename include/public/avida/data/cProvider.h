/*
 *  data/cProvider.h
 *  avida-core
 *
 *  Created by David on 5/16/11.
 *  Copyright 2011 Michigan State University. All rights reserved.
 *  http://avida.devosoft.org/
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
 *  Authors: David M. Bryson <david@programerror.com>
 *
 */

#ifndef AvidaDataProvider_h
#define AvidaDataProvider_h

#include "apto/core/Set.h"
#include "apto/core/String.h"
#include "apto/platform.h"


namespace Avida {
  namespace Data {
    
    // cProvider - Data Provider Protocol Definition
    // --------------------------------------------------------------------------------------------------------------
    
    class cProvider
    {
    public:
      virtual LIB_EXPORT ~cProvider() { ; }
      
      virtual LIB_EXPORT const Apto::Set<Apto::String>& Provides() = 0;

      virtual LIB_EXPORT void UpdateState() = 0;
    };
    
  };
};

#endif
