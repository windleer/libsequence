#include <Sequence/SimData.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
// For parallelizing nSL:
#include <functional>
#include <iostream>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>
using namespace std;

namespace
{
    double
    update_s2(std::string::const_iterator start,
              std::string::const_iterator left,
              std::string::const_iterator right, const Sequence::SimData &d,
              const std::unordered_map<double, double> &gmap)
    {
        auto p1 = d.position(
            std::vector<double>::size_type(distance(start, right)) - 1);
        auto p2 = d.position(
            std::vector<double>::size_type(distance(start, left)));
        if (gmap.empty())
            {
                // return phyisical distance
                return fabs(p1 - p2);
            }
        // return distance along genetic map,
        // in whatever units those are.
        auto fp1 = gmap.find(p1);
        auto fp2 = gmap.find(p2);
        if (fp1 == gmap.end() || fp2 == gmap.end())
            {
                throw std::runtime_error(
                    "position could not be found in genetic map, "
                    + std::string(__FILE__) + " line "
                    + std::to_string(__LINE__));
            }
        return fabs(fp1->second - fp2->second);
    }
    /*
      Mechanics of the nSL statistic

      RV = nSL,iHS, as defined in doi:10.1093/molbev/msu077
    */
    std::array<double, 4>
    __nlSsum(const unsigned &core, const Sequence::SimData &d,
             // const vector<size_t> &coretype,
             const std::unordered_map<double, double> &gmap)
    {
        auto csize = d.size();
        // This tracks s,s2 for ancestral and derived
        // mutation, resp:
        std::array<double, 4> rv = { 0., 0., 0., 0. };
        // number of comparisons for ancestral and
        // derived, resp:
        std::array<unsigned, 2> nc = { 0u, 0u };
        for (size_t i = 0; i < csize; ++i)
            {
                auto start = d[i].cbegin();
                auto bi = start + core;
                size_t iIsDer = (*bi == '1');
                for (size_t j = i + 1; j < csize; ++j)
                    {
                        auto bj = d[j].cbegin() + core;
                        size_t jIsDer = (*bj == '1');
                        if (iIsDer == jIsDer)
                            {
                                auto eri = d[i].crend();
                                auto ei = d[i].cend();
                                auto right = mismatch(bi, ei, bj);
                                string::const_reverse_iterator ri1(bi),
                                    ri2(bj);
                                auto left = mismatch(ri1, eri, ri2);
                                if (left.first != eri && right.first != ei)
                                    {
                                        rv[2 * iIsDer] += static_cast<double>(
                                            distance(left.first.base(),
                                                     right.first)
                                            + 1);
                                        rv[2 * iIsDer + 1] += update_s2(
                                            start, left.first.base(),
                                            right.first, d, gmap);
                                        nc[iIsDer]++;
                                    }
                            }
                    }
            }
        rv[0] /= static_cast<double>(nc[0]);
        rv[1] /= static_cast<double>(nc[0]);
        rv[2] /= static_cast<double>(nc[1]);
        rv[3] /= static_cast<double>(nc[1]);
        return rv;
    }

    struct nSLtask
    {
        using value_type = std::array<double, 4>;
        value_type value;
        unsigned core;
        nSLtask(unsigned core_)
            : value{ numeric_limits<double>::quiet_NaN(),
                     numeric_limits<double>::quiet_NaN(),
                     numeric_limits<double>::quiet_NaN(),
                     numeric_limits<double>::quiet_NaN() },
              core{ core_ }
        {
        }
        void
        operator()(const Sequence::SimData &d,
                   const std::unordered_map<double, double> &gmap)
        {
            value = __nlSsum(core, d, gmap);
        }

        value_type
        get() const
        {
            return value;
        }
    };
}

namespace Sequence
{
    /*
      The nSL statistic of doi:10.1093/molbev/msu077
    */
    pair<double, double>
    nSL(const unsigned &core, const SimData &d,
        const std::unordered_map<double, double> &gmap
        = std::unordered_map<double, double>())
    {
        auto nsl = __nlSsum(core, d, gmap);
        return make_pair(log(nsl[0]) - log(nsl[2]), log(nsl[1]) - log(nsl[3]));
    }

    vector<pair<double, double>>
    nSL_t(const SimData &d, const int nthreads = 1,
          const std::unordered_map<double, double> &gmap
          = std::unordered_map<double, double>())
    {
        tbb::task_scheduler_init init(nthreads);
        vector<nSLtask> tasks;
        for (unsigned i = 0; i < d.numsites(); ++i)
            {
                tasks.emplace_back(nSLtask(i));
            }
        tbb::parallel_for(
            tbb::blocked_range<std::size_t>(0, tasks.size()),
            [&tasks, &d, &gmap](const tbb::blocked_range<std::size_t> &r) {
                for (std::size_t i = r.begin(); i < r.end(); ++i)
                    tasks[i](d, gmap);
            });

        vector<pair<double, double>> rv;
        rv.reserve(tasks.size());
        for (auto &&t : tasks)
            {
                auto temp = t.get();
                rv.emplace_back(log(temp[0]) - log(temp[2]),
                                log(temp[1]) - log(temp[3]));
            }
        return rv;
    }
    /*
      Return max. abs value of standardized nSL and iHS, with the latter as
      defined by Ferrer-Admetella et al.
    */
    pair<double, double>
    snSL(const SimData &d, const double minfreq, const double binsize,
         const int nthreads = 1, const std::unordered_map<double, double> &gmap
                                 = std::unordered_map<double, double>())
    {
        if (d.empty())
            return make_pair(std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::quiet_NaN());
        vector<polymorphicSite> filtered;
        vector<unsigned> dcounts;
        dcounts.reserve(d.numsites());
        for_each(d.sbegin(), d.send(), [&](const polymorphicSite &p) {
            unsigned dcount = static_cast<unsigned>(
                count(p.second.begin(), p.second.end(), '1'));
            if (dcount && dcount < d.size())
                {
                    double f = static_cast<double>(dcount)
                               / static_cast<double>(d.size());
                    if (min(f, 1. - f) >= minfreq)
                        {
                            filtered.push_back(p);
                            dcounts.push_back(dcount);
                        }
                }
        });
        if (filtered.empty())
            return make_pair(std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::quiet_NaN());
        SimData __filtered(filtered.begin(), filtered.end());
        // Get the stats
        auto nSLstats = nSL_t(__filtered, nthreads, gmap);
        // Associate the stats with their DAFs
        using pp = pair<double, pair<double, double>>;
        vector<pp> binning;
        for (unsigned i = 0; i < __filtered.numsites(); ++i)
            {
                // pair<double, double> rvi = nSL(i, __filtered, gmap);
                binning.push_back(
                    make_pair(static_cast<double>(dcounts[i])
                                  / static_cast<double>(__filtered.size()),
                              nSLstats[i]));
            }
        // sort based on DAF
        std::sort(binning.begin(), binning.end(),
                  [](const pp &a, const pp &b) { return a.first < b.first; });
        double rv = std::numeric_limits<double>::quiet_NaN(),
               rv2 = std::numeric_limits<double>::quiet_NaN();
        // Now, bin, standardise, and move on...
        vector<pp> thisbin;
        auto bstart = binning.begin();
        size_t ttlSNPs = 0;
        for (double l = minfreq; l < 1.; l += binsize)
            {
                thisbin.clear();
                auto first = lower_bound(
                    bstart, binning.end(), l,
                    [](const pp &p, const double d) { return p.first < d; });
                auto last = upper_bound(
                    first, binning.end(), l + binsize,
                    [](const double d, const pp &p) { return d <= p.first; });
                for_each(first, last, [l, binsize](const pp &p) {
                    if (p.first < l || p.first >= l + binsize)
                        {
                            throw runtime_error("fuck off");
                        }
                });
                thisbin.insert(thisbin.end(), first, last);
                ttlSNPs += thisbin.size();
                bstart = last;
                /*
copy_if(binning.begin(), binning.end(), back_inserter(thisbin),
        [&](const pair<double, pair<double, double>> &data) {
            return isfinite(data.second.first)
                   && data.first >= l
                   && data.first < l + binsize;
        });
                */
                if (thisbin.size() > 1) // otherwise SD = 0, so there's
                                        // nothing to standardize
                    {
                        double mean1 = 0., mean2 = 0.;
                        for (const auto &p : thisbin)
                            {
                                if (isfinite(p.second.first))
                                    mean1 += p.second.first;
                                if (isfinite(p.second.second))
                                    mean2 += p.second.second;
                            }
                        mean1 /= static_cast<double>(thisbin.size());
                        mean2 /= static_cast<double>(thisbin.size());
                        double var1 = 0., var2 = 0.;
                        for (const auto &p : thisbin)
                            {
                                if (isfinite(p.second.first))
                                    var1 += pow(p.second.first - mean1, 2.0);
                                if (isfinite(p.second.first))
                                    var2 += pow(p.second.second - mean2, 2.0);
                            }
                        var1 /= static_cast<double>(thisbin.size() - 1);
                        var2 /= static_cast<double>(thisbin.size() - 1);
                        double sd1 = sqrt(var1), sd2 = sqrt(var2);
                        for_each(
                            thisbin.begin(), thisbin.end(),
                            [&](const pair<double, pair<double, double>>
                                    &data) {
                                double z1
                                    = (isfinite(sd1))
                                          ? (data.second.first - mean1) / sd1
                                          : numeric_limits<double>::
                                                quiet_NaN(),
                                    z2
                                    = (isfinite(sd2))
                                          ? (data.second.second - mean2) / sd2
                                          : numeric_limits<double>::
                                                quiet_NaN();
                                // Get max abs val of each stat
                                if (isfinite(z1)
                                    && (!isfinite(rv) || fabs(z1) > fabs(rv)))
                                    rv = z1;
                                if (isfinite(z2) && (!isfinite(rv2)
                                                     || fabs(z2) > fabs(rv2)))
                                    rv2 = z2;
                            });
                    }
            }
        if (ttlSNPs != __filtered.numsites())
            {
                throw runtime_error("Incorrect number of SNPs processed");
            }
        return make_pair(rv, rv2);
    }
}
