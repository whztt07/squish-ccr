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

#include "palettenormalfit.h"
#include "paletteset.h"
#include "paletteblock.h"

#include "inlineables.cpp"

namespace squish {

/* *****************************************************************************
 */
#if	!defined(SQUISH_USE_PRE)
PaletteNormalFit::PaletteNormalFit(PaletteSet const* palette, int flags, int swap, int shared)
  : PaletteSingleMatch(palette, flags, swap, shared)
  ,         PaletteFit(palette, flags, swap, shared)
{
  // the alpha-set (in theory we can do separate alpha + separate partitioning, but's not codeable)
  int const isets = m_palette->GetSets();
  int const asets = m_palette->IsSeperateAlpha() ? isets : 0;
  
  assume((isets >  0) && (isets <= 3));
  assume((asets >= 0) && (asets <= 3));
  assume(((isets    +    asets) <= 3));

  for (int s = 0; s < isets; s++) {
    // cache some values
    int const count = m_palette->GetCount(s);
    Vec4 const* values = m_palette->GetPoints(s);
    Vec4 const* weights = m_palette->GetWeights(s);

    // we don't do this for sparse sets
    if (count != 1) {
      Sym2x2 covariance;
      Vec4 centroid;
      Vec4 principle;

      // get the covariance matrix
      if (m_palette->IsUnweighted(s))
	ComputeWeightedCovariance2(covariance, centroid, count, values, m_metric[s]);
      else
	ComputeWeightedCovariance2(covariance, centroid, count, values, m_metric[s], weights);

      // compute the principle component
      GetPrincipleComponent(covariance, principle);

      // get the min and max normal as the codebook endpoints
      Vec4 start(0.0f);
      Vec4 end(0.0f);

      if (count > 0) {
	// compute the normal
	start = end = values[0];

	Scr4 min, max; min = max = Dot(values[0], principle);
	Vec4 mmn, mmx; mmn = mmx =     values[0];

	for (int i = 1; i < count; ++i) {
	  Scr4 val = Dot(values[i], principle);

	  if (min > val) {
	    start = values[i];
	    min   = val;
	  }
	  else if (max < val) {
	    end = values[i];
	    max = val;
	  }

	  mmn = Min(mmn, values[i]);
	  mmx = Max(mmx, values[i]);
	}

	// exclude z/alpha from PCA
	start = TransferZW(start, mmn);
	end   = TransferZW(end  , mmx);
      }

      // clamp the output to [0, 1]
      m_start[s] = start;
      m_end  [s] = end;
    }
  }

  // the alpha-set (in theory we can do separate alpha + separate partitioning, but's not codeable)
  for (int a = isets; a < (isets + asets); a++) {
    // cache some values
    int const count = m_palette->GetCount(a);
    Vec4 const* values = m_palette->GetPoints(a);

    // we don't do this for sparse sets
    if (count != 1) {
      // get the min and max normal as the codebook endpoints
      Vec4 start(1.0f);
      Vec4 end(1.0f);

      if (count > 0) {
	// compute the normal
	start = end = values[0];

	for (int i = 1; i < count; ++i) {
	  start = Min(start, values[i]);
	  end   = Max(end  , values[i]);
	}
      }

      // clamp the output to [0, 1]
      m_start[a] = start;
      m_end  [a] = end;
    }
  }
}

void PaletteNormalFit::Compress(void* block, vQuantizer &q, int mode)
{
  int ib = GetIndexBits(mode);
  int jb = ib >> 16; ib = ib & 0xFF;
  int cb = GetPrecisionBits(mode);
  int ab = cb >> 16; cb = cb & 0xFF;
  int zb = GetSharedField();

  q.ChangeShared(cb, cb, cb, ab, zb);

  // match each point to the closest code
  Scr4 error = Scr4(0.0f);
  a16 u8 closest[4][16];

  // the alpha-set (in theory we can do separate alpha + separate partitioning, but's not codeable)
  int const isets = m_palette->GetSets();
  int const asets = m_palette->IsSeperateAlpha() ? isets : 0;
  u8  const tmask = m_palette->IsMergedAlpha() ? 0xFF : 0x00;
  
  assume((isets >  0) && (isets <= 3));
  assume((asets >= 0) && (asets <= 3));
  assume(((isets    +    asets) <= 3));

  // create a codebook
  Vec4 codes[1 << 4];

  // loop over all sets
  for (int s = 0, sb = zb; s < (isets + asets); s++, sb >>= 1) {
    // how big is the codebook for the current set
    // swap the code-book when the swap-index bit is set
    int kb = ((s < isets) ^ (!!m_swapindex)) ? ib : jb;

    // cache some values
    int const count = m_palette->GetCount(s);
    Vec4 const* values = m_palette->GetPoints(s);
    u8 const* freq = m_palette->GetFrequencies(s);

    // in case of separate alpha the colors of the alpha-set have all been set to alpha
    Vec4 metric = m_metric[s < isets ? 0 : 1];

    // we do single entry fit for sparse sets
    if (count == 1) {
      // clear alpha-weight if alpha is disabled
      // in case of separate alpha the colors of the alpha-set have all been set to alpha
      u8 mask = ((s < isets) ? 0x7 : 0x8) | tmask;

      // find the closest code
      Scr4 dist = ComputeEndPoints(s, metric, cb, ab, sb, kb, mask);

      // save the index (it's just a single one)
      closest[s][0] = GetIndex();

      // accumulate the error
      error += dist * freq[0];
    }
    else {
      // snap floating-point-values to the integer-lattice
      Vec4 start = q.SnapToLattice(m_start[s], sb, 1 << SBSTART);
      Vec4 end   = q.SnapToLattice(m_end  [s], sb, 1 << SBEND);

      // TODO: pre-normalize codebook
      int ccs = CodebookP(codes, kb, start, end);

      for (int i = 0; i < count; ++i) {
	// find the closest code
	Scr4 dist = Scr4(-1.0f);
	Vec4 nval = Normalize(values[i]);
	int idx = 0;
	
	for (int j = 0; j < ccs; j += 0) {
	  Scr4 a0 = Dot(nval, Normalize(codes[j + 0]));
	  Scr4 a1 = Dot(nval, Normalize(codes[j + 1]));
	  Scr4 a2 = Dot(nval, Normalize(codes[j + 2]));
	  Scr4 a3 = Dot(nval, Normalize(codes[j + 3]));
	  
	  // measure angle-deviation (cosine)
	  Scr4 d0 = a0 * a0;
	  Scr4 d1 = a1 * a1;
	  Scr4 d2 = a2 * a2;
	  Scr4 d3 = a3 * a3;

	  // encourage OoO
	  Scr4 da = Max(d0, d1);
	  Scr4 db = Max(d2, d3);
	  dist = Max(da, dist);
	  dist = Max(db, dist);

	  // will cause VS to make them all cmovs
	  if (d0 == dist) { idx = j; } j++;
	  if (d1 == dist) { idx = j; } j++;
	  if (d2 == dist) { idx = j; } j++;
	  if (d3 == dist) { idx = j; } j++;
	}

	// save the index
	closest[s][i] = (u8)idx;

	// accumulate the error (sine)
	error += (Scr4(1.0f) - dist) * freq[i];
      }
    }

    // kill early if this scheme looses
    if (!(error < m_besterror))
      return;
  }

  // because the original alpha-channel's weight was killed it is completely random and need to be set to 1.0f
  if (!m_palette->IsTransparent()) {
    switch (m_palette->GetRotation()) {
      default: for (int a = 0; a < isets + asets; a++) m_start[a].Set<3>(1.0f), m_end[a].Set<3>(1.0f); break;
      case  1: for (int a = 0; a <         isets; a++) m_start[a].Set<0>(1.0f), m_end[a].Set<0>(1.0f); break;
      case  2: for (int a = 0; a <         isets; a++) m_start[a].Set<1>(1.0f), m_end[a].Set<1>(1.0f); break;
      case  3: for (int a = 0; a <         isets; a++) m_start[a].Set<2>(1.0f), m_end[a].Set<2>(1.0f); break;
    }
  }

  // remap the indices
  for (int s = 0;     s <  isets         ; s++)
    m_palette->RemapIndices(closest[s], m_indices[0], s);
  for (int a = isets; a < (isets + asets); a++) {
    m_palette->RemapIndices(closest[a], m_indices[1], a);

    // copy alpha into the common start/end definition
    m_start[a - isets] = TransferW(m_start[a - isets], m_start[a]);
    m_end  [a - isets] = TransferW(m_end  [a - isets], m_end  [a]);
  }

  typedef u8 (&Itwo)[2][16];
  typedef u8 (&Ione)[1][16];

  typedef Vec4 (&V4thr)[3];
  typedef Vec4 (&V4two)[2];
  typedef Vec4 (&V4one)[1];

  // save the block
  int partition = m_palette->GetPartition();
  int rot = m_palette->GetRotation(), sel = m_swapindex;
  switch (mode) {
    case 0: WritePaletteBlock3_m1(partition, (V4thr)m_start, (V4thr)m_end, m_sharedbits, (Ione)m_indices, block); break;
    case 1: WritePaletteBlock3_m2(partition, (V4two)m_start, (V4two)m_end, m_sharedbits, (Ione)m_indices, block); break;
    case 2: WritePaletteBlock3_m3(partition, (V4thr)m_start, (V4thr)m_end, m_sharedbits, (Ione)m_indices, block); break;
    case 3: WritePaletteBlock3_m4(partition, (V4two)m_start, (V4two)m_end, m_sharedbits, (Ione)m_indices, block); break;
    case 4: WritePaletteBlock4_m5(rot, sel , (V4one)m_start, (V4one)m_end, m_sharedbits, (Itwo)m_indices, block); break;
    case 5: WritePaletteBlock4_m6(rot,       (V4one)m_start, (V4one)m_end, m_sharedbits, (Itwo)m_indices, block); break;
    case 6: WritePaletteBlock4_m7(partition, (V4one)m_start, (V4one)m_end, m_sharedbits, (Ione)m_indices, block); break;
    case 7: WritePaletteBlock4_m8(partition, (V4two)m_start, (V4two)m_end, m_sharedbits, (Ione)m_indices, block); break;
  }

  // save the error
  m_besterror = error;
  m_best = true;
}
#endif

/* *****************************************************************************
 */
#if	defined(SQUISH_USE_AMP) || defined(SQUISH_USE_COMPUTE)
#endif

} // namespace squish
