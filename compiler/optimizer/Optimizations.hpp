/*******************************************************************************
 * Copyright IBM Corp. and others 2000
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#ifndef TR_OPTIMIZATIONS_INCL
#define TR_OPTIMIZATIONS_INCL

namespace OMR
   {
      // Note: Optimizations enum needs to match optimizer_name array
      // defined in compiler/optimizer/OMROptimizer.cpp
      enum Optimizations
      {
      #define OPTIMIZATION(name) name,
      #define OPTIMIZATION_ENUM_ONLY(entry) entry,
      #include "optimizer/Optimizations.enum"
      OPTIMIZATION_ENUM_ONLY(numOpts)  // after all project-specific optimization enums
      #include "optimizer/OptimizationGroups.enum"
      OPTIMIZATION_ENUM_ONLY(numGroups)  // after all project-specific optimization group enums
      #undef OPTIMIZATION
      #undef OPTIMIZATION_ENUM_ONLY
      };
   }

#endif
