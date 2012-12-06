/* -*- C++ -*- */
/**
 * @copyright
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 * @endcopyright
 */

#ifndef __cplusplus
#error "This is a C++ header file."
#endif

#ifndef SVN_CXXHL_TYPES_TRISTATE_H
#define SVN_CXXHL_TYPES_TRISTATE_H

namespace apache {
namespace subversion {
namespace cxxhl {

class Tristate
{
  public:
    Tristate(const Tristate& that) : m_value(that.m_value) {}

    bool operator==(const Tristate& that) { return m_value == that.m_value; }
    bool operator!=(const Tristate& that) { return !(*this == that); }

    static const Tristate TRUE;
    static const Tristate FALSE;
    static const Tristate UNKNOWN;

  private:
    explicit Tristate(short value);
    short m_value;
};

} // namespace cxxhl
} // namespace subversion
} // namespace apache

#endif  // SVN_CXXHL_TYPES_TRISTATE_H