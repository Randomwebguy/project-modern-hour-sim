// SPDX-License-Identifier: GPL-3.0-or-later
// Unchanged pre-perf80 native depth reference, retained only in boundary checks.
struct DepthTri80
{
	unsigned int i;
	unsigned int j;
	unsigned int k;
};

struct DepthEntry80
{
	DepthTri80 tri;
	unsigned int idx;
	float z;
};

bool operator <(const DepthEntry80 &l, const DepthEntry80 &r) { return l.z < r.z; }
bool operator <=(const DepthEntry80 &l, const DepthEntry80 &r) { return l.z <= r.z; }
bool operator >(const DepthEntry80 &l, const DepthEntry80 &r) { return l.z > r.z; }
bool operator >=(const DepthEntry80 &l, const DepthEntry80 &r) { return l.z >= r.z; }
bool operator ==(const DepthEntry80 &l, const DepthEntry80 &r) { return l.z == r.z; }
// ----------------------------------------------------------------------------
static
void LegacyInsertion80(DepthEntry80 *begin, DepthEntry80 *end)
{
	for (DepthEntry80 *iter = begin + 1; iter < end; ++iter) {
		DepthEntry80 val = iter[0];
		DepthEntry80 *insert = iter;
		while (insert != begin && insert[-1] > val) {
			insert[0] = insert[-1];
			insert -= 1;
		}
		insert[0] = val;
	}
}

// ----------------------------------------------------------------------------
static
void LegacyDepthSort80(DepthEntry80 *begin, DepthEntry80 *end)
{
	const auto diff = end - begin;
	if (diff <= 16) {
		// Insertion sort has less overhead for small arrays
		LegacyInsertion80(begin, end);
	} else {
		// Choose the median of begin, mid, and (end - 1) as the partitioning element.
		// Rearrange so that *(begin + 1) <= *begin <= *(end - 1).  These will be guard
		// elements.
		DepthEntry80 *mid = begin + diff/2;
		std::swap(mid[0], begin[1]);
		if (begin[1] > end[-1]) {
			std::swap(begin[1], end[-1]);
		}
		if (begin[0] > end[-1]) {
			std::swap(begin[0], end[-1]);
		}
		if (begin[1] > begin[0]) {
			std::swap(begin[1], begin[0]);
		}

		// *begin is now the partitioning element
		DepthEntry80 *begin1 = begin + 1;	// TODO: Temp fix until I find out who is passing me NaN
		DepthEntry80 *end1 = end - 1;			// TODO: Temp fix until I find out who is passing me NaN
		DepthEntry80 *left = begin + 1;
		DepthEntry80 *right = end - 1;
		for (;;) {
#if 0		// TODO: Temp fix until I find out who is passing me NaN.
			do ++left; while (left[0] < begin[0]);		// Scan up to find element >= than partition
			do --right; while (right[0] > begin[0]);	// Scan down to find element <= than partition
#else
			do ++left; while (left < end1 && left[0] < begin[0]);		// Scan up to find element >= than partition
			do --right; while (right > begin1 && right[0] > begin[0]);	// Scan down to find element <= than partition
#endif
			if (right < left) break;									// Pointers crossed.  Partitioning completed.
			std::swap(left[0], right[0]);							// Exchange elements.
		}
		std::swap(begin[0], right[0]);							// Insert partition element

		// LegacyDepthSort80 the smaller subarray first then the larger
		if (right - begin > end - (right + 1)) {
			LegacyDepthSort80(right + 1, end);
			LegacyDepthSort80(begin, right);
		} else {
			LegacyDepthSort80(begin, right);
			LegacyDepthSort80(right + 1, end);
		}
	}
}

// ----------------------------------------------------------------------------

