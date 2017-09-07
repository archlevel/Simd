/*
* Simd Library (http://ermig1979.github.io/Simd).
*
* Copyright (c) 2011-2017 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdMemory.h"
#include "Simd/SimdStore.h"
#include "Simd/SimdCompare.h"
#include "Simd/SimdLog.h"

namespace Simd
{
#ifdef SIMD_AVX512BW_ENABLE
    namespace Avx512bw
    {
        namespace
        {
            template<class T> struct Buffer
            {
                Buffer(size_t rowSize, size_t histogramSize)
                {
                    _p = Allocate(sizeof(T)*rowSize + 4*sizeof(uint32_t)*histogramSize);
                    v = (T*)_p;
                    h[0] = (uint32_t *)(v + rowSize);
                    h[1] = h[0] + histogramSize;
                    h[2] = h[1] + histogramSize;
                    h[3] = h[2] + histogramSize;
                    memset(h[0], 0, 4*sizeof(uint32_t)*histogramSize);
                }

                ~Buffer()
                {
                    Free(_p);
                }

                T * v;
                uint32_t * h[4];
            private:
                void *_p;
            };
        }

        template <bool srcAlign, bool stepAlign> SIMD_INLINE __m512i AbsSecondDerivative(const uint8_t * src, ptrdiff_t step)
        {
            const __m512i s0 = Load<srcAlign && stepAlign>(src - step);
            const __m512i s1 = Load<srcAlign>(src);
            const __m512i s2 = Load<srcAlign && stepAlign>(src + step);
            return AbsDifferenceU8(_mm512_avg_epu8(s0, s2), s1);
        }

        template <bool align> SIMD_INLINE void AbsSecondDerivative(const uint8_t * src, ptrdiff_t colStep, ptrdiff_t rowStep, uint8_t * dst)
        {
            const __m512i sdX = AbsSecondDerivative<align, false>(src, colStep);
            const __m512i sdY = AbsSecondDerivative<align, true>(src, rowStep);
            Store<align>(dst, _mm512_max_epu8(sdY, sdX));
        }

        SIMD_INLINE void SumHistograms(uint32_t * src, size_t start, uint32_t * dst)
        {
            uint32_t * src0 = src + start;
            uint32_t * src1 = src0 + start + HISTOGRAM_SIZE;
            uint32_t * src2 = src1 + start + HISTOGRAM_SIZE;
            uint32_t * src3 = src2 + start + HISTOGRAM_SIZE;
            for(size_t i = 0; i < HISTOGRAM_SIZE; i += F)
                Store<false>(dst + i, _mm512_add_epi32(_mm512_add_epi32(Load<true>(src0 + i), Load<true>(src1 + i)), _mm512_add_epi32(Load<true>(src2 + i), Load<true>(src3 + i))));
        }

#ifdef __GNUC__
//#define SIMD_USE_GATHER_AND_SCATTER_FOR_HISTOGRAM // low performance
#endif

#if defined(SIMD_USE_GATHER_AND_SCATTER_FOR_HISTOGRAM)
		const __m512i K32_TO_HISTOGRAMS = SIMD_MM512_SETR_EPI32(0x000, 0x100, 0x200, 0x300, 0x000, 0x100, 0x200, 0x300, 0x000, 0x100, 0x200, 0x300, 0x000, 0x100, 0x200, 0x300);

		SIMD_INLINE void AddToHistogram(__m128i index, uint32_t * histogram)
		{
			__m128i hist = _mm_i32gather_epi32((int*)histogram, index, 4);
			hist = _mm_add_epi32(hist, Sse2::K32_00000001);
			_mm_i32scatter_epi32((int*)histogram, index, hist, 4);
		}
#endif

        template<bool align> void AbsSecondDerivativeHistogram(const uint8_t *src, size_t width, size_t height, size_t stride,
            size_t step, size_t indent, uint32_t * histogram)
        {
            Buffer<uint8_t> buffer(AlignHi(width, A), HISTOGRAM_SIZE);
            buffer.v += indent;
            src += indent*(stride + 1);
            height -= 2*indent;
            width -= 2*indent;

            ptrdiff_t bodyStart = (uint8_t*)AlignHi(buffer.v, A) - buffer.v;
            ptrdiff_t bodyEnd = bodyStart + AlignLo(width - bodyStart, A);
            size_t rowStep = step*stride;
            size_t alignedWidth = Simd::AlignLo(width, 4);
			size_t fullAlignedWidth = Simd::AlignLo(width, Sse2::A);
			for(size_t row = 0; row < height; ++row)
            {
                if(bodyStart)
                    AbsSecondDerivative<false>(src, step, rowStep, buffer.v);
                for(ptrdiff_t col = bodyStart; col < bodyEnd; col += A)
                    AbsSecondDerivative<align>(src + col, step, rowStep, buffer.v + col);
                if(width != (size_t)bodyEnd)
                    AbsSecondDerivative<false>(src + width - A, step, rowStep, buffer.v + width - A);

                size_t col = 0;
#if defined(SIMD_USE_GATHER_AND_SCATTER_FOR_HISTOGRAM)
				for (; col < fullAlignedWidth; col += Sse2::A)
				{
					__m512i index = _mm512_add_epi32(_mm512_cvtepu8_epi32(Sse2::Load<false>((__m128i*)(buffer.v + col))), K32_TO_HISTOGRAMS);
					AddToHistogram(_mm512_extracti32x4_epi32(index, 0), buffer.h[0]);
					AddToHistogram(_mm512_extracti32x4_epi32(index, 1), buffer.h[0]);
					AddToHistogram(_mm512_extracti32x4_epi32(index, 2), buffer.h[0]);
					AddToHistogram(_mm512_extracti32x4_epi32(index, 3), buffer.h[0]);
			    }
#endif
                for(; col < alignedWidth; col += 4)
                {
                    ++buffer.h[0][buffer.v[col + 0]];
                    ++buffer.h[1][buffer.v[col + 1]];
                    ++buffer.h[2][buffer.v[col + 2]];
                    ++buffer.h[3][buffer.v[col + 3]];
                }
                for(; col < width; ++col)
                    ++buffer.h[0][buffer.v[col + 0]];
                src += stride;
            }

            SumHistograms(buffer.h[0], 0, histogram);
        }

        void AbsSecondDerivativeHistogram(const uint8_t *src, size_t width, size_t height, size_t stride,
            size_t step, size_t indent, uint32_t * histogram)
        {
            assert(width > 2*indent && height > 2*indent && indent >= step && width >= A + 2*indent);

            if(Aligned(src) && Aligned(stride))
                AbsSecondDerivativeHistogram<true>(src, width, height, stride, step, indent, histogram);
            else
                AbsSecondDerivativeHistogram<false>(src, width, height, stride, step, indent, histogram);
        }
    }
#endif// SIMD_AVX512BW_ENABLE
}
