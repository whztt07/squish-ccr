/* -----------------------------------------------------------------------------

	Copyright (c) 2006 Simon Brown                          si@sjbrown.co.uk
	Copyright (c) 2012 Niels Fr�hling              niels@paradice-insight.us

	Permission is hereby granted, free of charge, to any person obtaining
	a copy of this software and associated documentation files (the
	"Software"), to	deal in the Software without restriction, including
	without limitation the rights to use, copy, modify, merge, publish,
	distribute, sublicense, and/or sell copies of the Software, and to
	permit persons to whom the Software is furnished to do so, subject to
	the following conditions:

	The above copyright notice and this permission notice shall be included
	in all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
	OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
	CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
	TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   -------------------------------------------------------------------------- */

#include "bitonenormalfit.h"
#include "bitoneset.h"
#include "bitoneblock.h"

#include "inlineables.cpp"

namespace squish {
  
#ifdef	FEATURE_NORMALFIT_UNITGUARANTEE
#define DISARM	true
#else
#define DISARM	false
#endif

/* *****************************************************************************
 */
#if	!defined(SQUISH_USE_PRE)
BitoneNormalFit::BitoneNormalFit(BitoneSet const* bitones, int flags)
  : BitoneFit(bitones, flags)
{
  // initialize the best error
  m_besterror = Scr3(FLT_MAX);

  // cache some values
  int const count = m_bitones->GetCount();
  Vec3 const* values = m_bitones->GetPoints();
  float const* weights = m_bitones->GetWeights();

#ifdef	FEATURE_NORMALFIT_PROJECT
  Sym3x3 covariance;
  Vec3 centroid;
  Vec3 principle;

  // get the covariance matrix
  if (m_bitones->IsUnweighted())
    ComputeWeightedCovariance3(covariance, centroid, count, values, Vec3(1.0f));
  else
    ComputeWeightedCovariance3(covariance, centroid, count, values, Vec3(1.0f), weights);

  // compute the principle component
  GetPrincipleComponent(covariance, principle);

  // get the min and max normal as the codebook endpoints
  Vec3 start(0.0f);
  Vec3 end(0.0f);

  if (count > 0) {
    const Vec3 scale  = Vec3( 1.0f / 0.5f);
    const Vec3 offset = Vec3(-1.0f * 0.5f);
    const Vec3 scalei = Vec3( 1.0f * 0.5f);

#undef	FEATURE_NORMALFIT_PROJECT_NEAREST
#ifdef	FEATURE_NORMALFIT_PROJECT_NEAREST
    Vec3 centroidn = (scale * (offset + centroid));
    Vec3 rec = Reciprocal(principle);
    Scr3 min, max;
    Vec3 chk;

    // http://geomalgorithms.com/a07-_distance.html
    // compute the line parameters of the two closest points
    min = Scr3( FLT_MAX);
    max = Scr3(-FLT_MAX);

    for (int i = 0; i < count; ++i) {
      Vec3 valuenorm = Normalize(scale * (offset + values[i]));

      Vec3 u = principle;//L1.P1 - L1.P0;
      Vec3 v = valuenorm;//L2.P1 - L2.P0;
      Vec3 w = centroidn;//L1.P0 - L2.P0;
      Scr3 a = Dot(u, u);         // always >= 0
      Scr3 b = Dot(u, v);
      Scr3 c = Dot(v, v);         // always >= 0
      Scr3 d = Dot(u, w);
      Scr3 e = Dot(v, w);
      Scr3 D = a * c - b * b;     // always >= 0
      Scr3 sc, tc;

      // compute the line parameters of the two closest points
      if (D < Scr3(0.00001f)) {    // the lines are almost parallel
	sc = Scr3(0.0f);	   // use the largest denominator
	tc = (b > c 
	  ? d * Reciprocal(b) 
	  : e * Reciprocal(c)
	);    
      }
      else {
	D = Reciprocal(D);

	sc = (b * e - c * d) * D;
	tc = (a * e - b * d) * D;
      }

      // one dimension of the principle axis is 1
      // the maximum magnitude the principle axis
      // can move in the [-1,+1] cube is 1.41*2
      // without leaving the cube's boundaries
      sc = Min(sc, Scr3( 2.82842712474619f));
      sc = Max(sc, Scr3(-2.82842712474619f));

      min = Min(min, sc);
      max = Max(max, sc);
    }
    
    start = centroidn + principle * min;
    end   = centroidn + principle * max;
    
    start = (start * scalei) - offset;
    end   = (end   * scalei) - offset;
#else
    Scr3 div = Reciprocal(Dot(principle, principle));
    Vec3 rec = Reciprocal(    principle            );
    Scr3 len, min, max;
    Vec3 chk;

    // compute the projection
    min = max = Dot(values[0] - centroid, principle);

    for (int i = 1; i < count; ++i) {
      len = Dot(values[i] - centroid, principle);
      min = Min(min, len);
      max = Max(max, len);
    }

    start = centroid + principle * min * div;
    end   = centroid + principle * max * div;
#endif

#if 1
    // intersect negative undershoot with axis-plane(s), clamp to 0.0
    chk = start;
    while (CompareAnyLessThan(chk, Vec3(-1.0f / 65536))) {
      Vec3 fct = chk * rec;
      Vec3 hin = Select(fct, chk, HorizontalMin(chk));

      start -= principle * hin;
      chk = start;
    }

    // intersect negative undershoot with axis-plane(s), clamp to 0.0
    chk = end;
    while (CompareAnyLessThan(chk, Vec3(-1.0f / 65536))) {
      Vec3 fct = chk * rec;
      Vec3 hin = Select(fct, chk, HorizontalMin(chk));

      end -= principle * hin;
      chk = end;
    }

    // intersect positive overshoot with axis-plane(s), clamp to 1.0
    chk = start - Vec3(1.0f);
    while (CompareAnyGreaterThan(chk, Vec3(1.0f / 65536))) {
      Vec3 fct = chk * rec;
      Vec3 hax = Select(fct, chk, HorizontalMax(chk));

      start -= principle * hax;
      chk = start - Vec3(1.0f);
    }

    // intersect positive overshoot with axis-plane(s), clamp to 1.0
    chk = end - Vec3(1.0f);
    while (CompareAnyGreaterThan(chk, Vec3(1.0f / 65536))) {
      Vec3 fct = chk * rec;
      Vec3 hax = Select(fct, chk, HorizontalMax(chk));

      end -= principle * hax;
      chk = end - Vec3(1.0f);
    }
#else
    start = (scale * (offset + start));
    end   = (scale * (offset + end  ));

    start = Normalize(start);
    end   = Normalize(end  );

    start = (start * scalei) - offset;
    end   = (end   * scalei) - offset;
#endif

/*  assert(HorizontalMin(start).X() > -0.0001);
    assert(HorizontalMin(end  ).X() > -0.0001);
    assert(HorizontalMax(start).X() <  1.0001);
    assert(HorizontalMax(end  ).X() <  1.0001);	 */
#else
    Scr3 min, max;

    // compute the normal
    start = end = values[0];
    min = max = Dot(values[0], principle);

    for (int i = 1; i < count; ++i) {
      Scr3 val = Dot(values[i], principle);

      if (val < min) {
	start = values[i];
	min = val;
      }
      else if (val > max) {
	end = values[i];
	max = val;
      }
    }
#endif
  }

  // snap floating-point-values to the integer-lattice and save
  m_start_candidate = Truncate(start * 255.0f) * (1.0f / 255.0f);
  m_end_candidate   = Truncate(end   * 255.0f) * (1.0f / 255.0f);
}

void BitoneNormalFit::kMeans4()
{
  const Vec3 scale  = Vec3( 1.0f / 0.5f);
  const Vec3 offset = Vec3(-1.0f * 0.5f);
  const Vec3 scalei = Vec3( 1.0f * 0.5f);
  
  // cache some values
  int const count = m_bitones->GetCount();
  Vec3 const* values = m_bitones->GetPoints();
  u8 const* freq = m_bitones->GetFrequencies();
  
  Vec3 c_start = m_start, c_end = m_end;
  Vec3 l_start = m_start, l_end = m_end;
  Scr3 berror = Scr3(16.0f);
  
  int trie = 1 + (m_flags & kColourIterativeClusterFits) / kColourClusterFit;
  do {
    Vec3 means[4];

    means[0] = Vec3(0.0f);
    means[1] = Vec3(0.0f);
    means[2] = Vec3(0.0f);
    means[3] = Vec3(0.0f);
    
    // create a codebook
    // resolve "metric * (value - code)" to "metric * value - metric * code"
    Vec3 codes[4]; Codebook4(codes, c_start, c_end);
    
    codes[0] = scale * (offset + codes[0]);
    codes[1] = scale * (offset + codes[1]);
    codes[2] = scale * (offset + codes[2]);
    codes[3] = scale * (offset + codes[3]);

    codes[0] = Complement<DISARM>(codes[0]);
    codes[1] = Complement<DISARM>(codes[1]);
    codes[2] = Complement<DISARM>(codes[2]);
    codes[3] = Complement<DISARM>(codes[3]);

    Scr3 merror = Scr3(16.0f);
    for (int i = 0; i < count; ++i) {
      // find the closest code
      Scr3 dist = Scr3(-1.0f);
      Vec3 value = Normalize(scale * (offset + values[i]));
      int idx = 0;

      {
	Scr3 d0 = Dot(value, codes[0]);
	Scr3 d1 = Dot(value, codes[1]);
	Scr3 d2 = Dot(value, codes[2]);
	Scr3 d3 = Dot(value, codes[3]);
	
	// select the smallest deviation (NaN as first arg is ignored!)
	dist = Max(d0, dist);
	dist = Max(d1, dist);
	dist = Max(d2, dist);
	dist = Max(d3, dist);

	// will cause VS to make them all cmovs
	if (d3 == dist) { idx = 3; }
	if (d2 == dist) { idx = 2; }
	if (d1 == dist) { idx = 1; }
	if (d0 == dist) { idx = 0; }
      }
      
      // accumulate the error
      means[idx] += value * freq[i];
      merror     -= dist  * freq[i];
    }
  
    if (berror > merror) {
      berror = merror;
      
      m_start = c_start;
      m_end   = c_end;
    }

    means[0] = (Normalize(means[0]) * scalei) - offset;
    means[1] = (Normalize(means[1]) * scalei) - offset;
    
    l_start = c_start;
    l_end   = c_end;
    c_start = Truncate(means[0] * 255.0f) * (1.0f / 255.0f);
    c_end   = Truncate(means[1] * 255.0f) * (1.0f / 255.0f);

  } while(--trie && !(CompareAllEqualTo(c_start, l_start) && CompareAllEqualTo(c_end, l_end)));
}

void BitoneNormalFit::Permute4()
{
  const Vec3 scale  = Vec3( 1.0f / 0.5f);
  const Vec3 offset = Vec3(-1.0f * 0.5f);
  const Vec3 scalei = Vec3( 1.0f * 0.5f);
  
  // cache some values
  int const count = m_bitones->GetCount();
  Vec3 const* values = m_bitones->GetPoints();
  u8 const* freq = m_bitones->GetFrequencies();
  
  Scr3 berror = Scr3(16.0f);
  
  Vec3 c_start = m_start;
  Vec3 c_end   = m_end;
  Scr3 l_start = LengthSquared(Normalize(scale * (offset + m_start)));
  Scr3 l_end   = LengthSquared(Normalize(scale * (offset + m_end)));
  Vec3 q_start = Reciprocal(Vec3(255.0f) + Vec3(1.0f));
  Vec3 q_end   = q_start;

  // adjust offset towards sphere-boundary (or center if DISARM)
  if (!(l_start < Scr3(1.0f)) || DISARM)
    q_start = Vec3(0.0f) - q_start;
  if (!(l_end   < Scr3(1.0f)) || DISARM)
    q_end   = Vec3(0.0f) - q_end;
  
  int trie = 0x3F;
  do {
    // permute end-points +-1 towards sphere-boundary
    Vec3 p_start = q_start & Vec3(!(trie & 0x01), !(trie & 0x02), !(trie & 0x04));
    Vec3 p_end   = q_end   & Vec3(!(trie & 0x08), !(trie & 0x10), !(trie & 0x20));
    
    p_start = Truncate((c_start + p_start) * 255.0f) * (1.0f / 255.0f);
    p_end   = Truncate((c_end   + p_end  ) * 255.0f) * (1.0f / 255.0f);

    // create a codebook
    // resolve "metric * (value - code)" to "metric * value - metric * code"
    Vec3 codes[4]; Codebook4(codes, p_start, p_end);
    
    codes[0] = scale * (offset + codes[0]);
    codes[1] = scale * (offset + codes[1]);
    codes[2] = scale * (offset + codes[2]);
    codes[3] = scale * (offset + codes[3]);

    codes[0] = Complement<DISARM>(codes[0]);
    codes[1] = Complement<DISARM>(codes[1]);
    codes[2] = Complement<DISARM>(codes[2]);
    codes[3] = Complement<DISARM>(codes[3]);

    Scr3 merror = Scr3(16.0f);
    for (int i = 0; i < count; ++i) {
      // find the closest code
      Scr3 dist = Scr3(-1.0f);
      Vec3 value = Normalize(scale * (offset + values[i]));

      {
	Scr3 d0 = Dot(value, codes[0]);
	Scr3 d1 = Dot(value, codes[1]);
	Scr3 d2 = Dot(value, codes[2]);
	Scr3 d3 = Dot(value, codes[3]);
	
	// select the smallest deviation (NaN as first arg is ignored!)
	dist = Max(d0, dist);
	dist = Max(d1, dist);
	dist = Max(d2, dist);
	dist = Max(d3, dist);
      }
      
      // accumulate the error
      merror -= dist * freq[i];
    }
    
    if (berror > merror) {
      berror = merror;

      m_start = p_start;
      m_end   = p_end;
    }

  } while(--trie);
}

void BitoneNormalFit::Compress4(void* block)
{
  const Vec3 scale  = Vec3( 1.0f / 0.5f);
  const Vec3 offset = Vec3(-1.0f * 0.5f);

  // cache some values
  int const count = m_bitones->GetCount();
  Vec3 const* values = m_bitones->GetPoints();
  u8 const* freq = m_bitones->GetFrequencies();
  
  // use a fitting algorithm
  m_start = m_start_candidate;
  m_end   = m_end_candidate;
  if (m_flags & kColourIterativeClusterFits)
    kMeans4();
//if ((m_flags & kColourIterativeClusterFits) >= (kColourIterativeClusterFit))
//  Permute4();
  
  // create a codebook
  // resolve "metric * (value - code)" to "metric * value - metric * code"
  Vec3 codes[4]; Codebook4(codes, m_start, m_end);
  
  codes[0] = scale * (offset + codes[0]);
  codes[1] = scale * (offset + codes[1]);
  codes[2] = scale * (offset + codes[2]);
  codes[3] = scale * (offset + codes[3]);

  codes[0] = Complement<DISARM>(codes[0]);
  codes[1] = Complement<DISARM>(codes[1]);
  codes[2] = Complement<DISARM>(codes[2]);
  codes[3] = Complement<DISARM>(codes[3]);

  // match each point to the closest code
  u8 closest[16];

  Scr3 error = Scr3(16.0f);
  for (int i = 0; i < count; ++i) {
    // find the closest code
    Scr3 dist = Scr3(-1.0f);
    Vec3 value = Normalize(scale * (offset + values[i]));
    int idx = 0;

    {
      Scr3 d0 = Dot(value, codes[0]);
      Scr3 d1 = Dot(value, codes[1]);
      Scr3 d2 = Dot(value, codes[2]);
      Scr3 d3 = Dot(value, codes[3]);

      // select the smallest deviation (NaN as first arg is ignored!)
      dist = Max(d0, dist);
      dist = Max(d1, dist);
      dist = Max(d2, dist);
      dist = Max(d3, dist);

      // will cause VS to make them all cmovs
      if (d3 == dist) { idx = 3; }
      if (d2 == dist) { idx = 2; }
      if (d1 == dist) { idx = 1; }
      if (d0 == dist) { idx = 0; }
    }

    // save the index
    closest[i] = (u8)idx;

    // accumulate the error
    error -= dist * freq[i];
  }

  // save this scheme if it wins
  if (error < m_besterror) {
    // save the error
    m_besterror = error;

    // remap the indices
    u8 indices[16];

    m_bitones->RemapIndices(closest, indices);

    // save the block
    WriteBitoneBlock4(m_start, m_end, indices, block);
  }
}
#endif

/* *****************************************************************************
 */
#if	defined(SQUISH_USE_AMP) || defined(SQUISH_USE_COMPUTE)
#endif

#undef DISARM

} // namespace squish
