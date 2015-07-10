/* 
 * dash-lib/GlobIter.h
 *
 * author(s): Karl Fuerlinger, LMU Munich 
 */
/* @DASH_HEADER@ */

#ifndef DASH__GLOB_ITER_H_
#define DASH__GLOB_ITER_H_

#include <dash/Pattern.h>
#include <dash/GlobRef.h>
#include <iostream>

namespace dash {

typedef long long gptrdiff_t;

template<
  typename ElementType,
  class PatternType = Pattern<1> >
class GlobIter : public std::iterator<
                          std::random_access_iterator_tag,
                          ElementType,
                          gptrdiff_t,
                          GlobIter<ElementType, PatternType>,
                          GlobRef<ElementType> > {
private:
  typedef GlobIter<ElementType, PatternType> self_t;

protected:
  GlobMem<ElementType> * m_globmem;
  const PatternType    * m_pattern;
  size_t                 m_idx;

  // For ostream output
  template<typename T_, class P_>
  friend std::ostream & operator<<(
      std::ostream& os, const GlobIter<T_, P_> & it);

public:
  /**
   * Default constructor.
   */
  GlobIter()
  : m_globmem(nullptr),
    m_pattern(nullptr),
    m_idx(0) {
  }

  /**
   * Constructor.
   */
  GlobIter(
    GlobMem<ElementType> * mem,
	  const PatternType    & pat,
	  size_t                 idx = 0)
  : m_globmem(mem), 
    m_pattern(&pat),
    m_idx(idx) {
  }

  GlobIter(
      const self_t & other) = default;
  GlobIter<ElementType, PatternType> & operator=(
      const self_t & other) = default;

  operator GlobPtr<ElementType>() const {
    auto glob_coords = m_pattern->coords(m_idx);
    auto local_pos   = m_pattern->at_unit(glob_coords);
    GlobPtr<ElementType> gptr =
      m_globmem->index_to_gptr(local_pos.unit, local_pos.index);
    return gptr;
  }

  /**
   * Dereference operator.
   *
   * \return  A global reference to the element at the iterator's position
   */
  GlobRef<ElementType> operator*() const {
    auto glob_coords = m_pattern->coords(m_idx);
    auto local_pos   = m_pattern->at_unit(glob_coords);
    DASH_LOG_TRACE_VAR("GlobIter.*", m_idx);
    DASH_LOG_TRACE_VAR("GlobIter.*", local_pos.unit);
    DASH_LOG_TRACE_VAR("GlobIter.*", local_pos.index);
    // Global pointer to element at given position:
    GlobPtr<ElementType> gptr =
      m_globmem->index_to_gptr(local_pos.unit, local_pos.index);
    // Global reference to element at given position:
    return GlobRef<ElementType>(gptr);
  }  

  /**
   * Subscript operator, returns global reference to element at given
   * global index.
   */
  GlobRef<ElementType> operator[](
    /// The global position of the element
    gptrdiff_t global_index) const {
    auto glob_coords = m_pattern->coords(global_index);
    auto local_pos   = m_pattern->at_unit(glob_coords);
    DASH_LOG_TRACE_VAR("GlobIter.[]", global_index);
    DASH_LOG_TRACE_VAR("GlobIter.[]", local_pos.unit);
    DASH_LOG_TRACE_VAR("GlobIter.[]", local_pos.index);
    // Global pointer to element at given position:
    GlobPtr<ElementType> gptr =
      m_globmem->index_to_gptr(local_pos.unit, local_pos.index);
    // Global reference to element at given position:
    return GlobRef<ElementType>(gptr);
  }

  /**
   * Checks whether the element referenced by this global iterator is in
   * the calling unit's local memory.
   */
  bool is_local() const {
    return m_pattern->is_local(m_idx);
  }

  /**
   * Global offset of the iterator within overall element range.
   */
  gptrdiff_t pos() const {
    return m_idx;
  }

  /**
   * The instance of \c GlobMem used by this iterator to resolve addresses
   * in global memory.
   */
  const GlobMem<ElementType> & globmem() const {
    return *m_globmem;
  }

  /**
   * Prefix increment operator.
   */
  self_t & operator++() {
    ++m_idx;
    return *this;
  }
  
  /**
   * Postfix increment operator.
   */
  self_t operator++(int) {
    self_t result = *this;
    ++m_idx;
    return result;
  }

  /**
   * Prefix decrement operator.
   */
  self_t & operator--() {
    --m_idx;
    return *this;
  }
  
  /**
   * Postfix decrement operator.
   */
  self_t operator--(int) {
    self_t result = *this;
    --m_idx;
    return result;
  }
  
  self_t & operator+=(gptrdiff_t n) {
    m_idx += n;
    return *this;
  }
  
  self_t & operator-=(gptrdiff_t n) {
    m_idx -= n;
    return *this;
  }

  self_t operator+(gptrdiff_t n) const {
    self_t res(
      m_globmem,
      *m_pattern,
      m_idx + static_cast<size_t>(n));
    return res;
  }

  self_t operator-(gptrdiff_t n) const {
    self_t res(
      m_globmem,
      *m_pattern,
      m_idx - static_cast<size_t>(n));
    return res;
  }

  gptrdiff_t operator+(
    const self_t & other) const {
    return m_idx + other.m_idx;
  }

  gptrdiff_t operator-(
    const self_t & other) const {
    return m_idx - other.m_idx;
  }

  bool operator<(const self_t & other) const {
    return m_idx < other.m_idx;
  }

  bool operator<=(const self_t & other) const {
    return m_idx <= other.m_idx;
  }

  bool operator>(const self_t & other) const {
    return m_idx > other.m_idx;
  }

  bool operator>=(const self_t & other) const {
    return m_idx >= other.m_idx;
  }

  bool operator==(const self_t & other) const {
    return m_idx == other.m_idx;
  }

  bool operator!=(const self_t & other) const {
    return m_idx != other.m_idx;
  }

  const PatternType & pattern() const {
    return *m_pattern;
  }
};

} // namespace dash

template<typename ElementType, class Pattern>
std::ostream & operator<<(
  std::ostream & os,
  const dash::GlobIter<ElementType, Pattern> & it) {
  dash::GlobPtr<ElementType> ptr(it); 
  os << "dash::GlobIter<ElementType, PatternType>: ";
  os << "idx=" << it.m_idx << std::endl;
  os << "--> " << ptr;
  return operator<<(os, ptr);
}

#endif // DASH__GLOB_ITER_H_
