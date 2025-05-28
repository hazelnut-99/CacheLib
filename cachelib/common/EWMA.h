#include <iostream>
#include <vector>
#include <cmath>
#include <limits> 

namespace facebook {
namespace cachelib {

class EWMA {
public:
    /**
     * @brief Constructor for the EWMA class.
     *
     * @param r Control parameter (learning rate), default 0.1.
     * @param L Control parameter (threshold), default 2.4.
     * @param burnin Number of initial observations before detecting changepoints, default 50.
     * @param mu Initial mean, default 0.0.
     * @param sigma Initial standard deviation, default 1.0.
     */
    EWMA(double r = 0.1, double L = 2.4, int burnin = 50, double mu = 0.0, double sigma = 1.0)
        : r_(r), L_(L), burnin_(burnin), mu_(mu), sigma_(sigma), Z_(mu), sigma_Z_(0.0), n_(2) {}

    /**
     * @brief Destructor for the EWMA class.
     * While not strictly necessary here, it's good practice to define it.
     */
    virtual ~EWMA() {}

    /**
     * @brief Update the mean and variance efficiently.
     *
     * @param data_new New observation.
     */
    void updateMeanVariance(double data_new) {
        double mu_new = mu_ + (data_new - mu_) / n_;
        sigma_ = std::sqrt(std::pow(sigma_, 2) + ((data_new - mu_) * (data_new - mu_new) - std::pow(sigma_, 2)) / n_);
        mu_ = mu_new;
    }

    /**
     * @brief Update the Z statistic and its standard deviation.
     *
     * @param i Time index.
     * @param data_new New observation.
     */
    void updateStatistics(int i, double data_new) {
        Z_ = (1 - r_) * Z_ + r_ * data_new;
        sigma_Z_ = sigma_ * std::sqrt((r_ / (2 - r_)) * (1 - std::pow((1 - r_), (2 * i))));
    }

    /**
    * @brief Decide whether or not a change has occurred.
    *
    * @param i Time index.
    * @return True if a change is detected, False otherwise.
    */
    bool decisionRule(int i) {
        if ((i >= burnin_) && (std::abs((Z_ - mu_) / L_) > sigma_Z_)) {
            changepoints_.push_back(i);
            n_ = 2;
            return true; // Indicate anomaly
        }
        else {
            n_++;
            return false; // Indicate no anomaly
        }
    }

    /**
     * @brief Reset the EWMA statistics to initial values.
     *
     * @param mu Initial mean (optional, defaults to the original mu_).
     * @param sigma Initial standard deviation (optional, defaults to the original sigma_).
     */
    void reset(double mu = 0.0, double sigma = 1.0) {
        mu_ = mu;
        sigma_ = sigma;
        Z_ = mu_;
        sigma_Z_ = 0.0;
        n_ = 2;
        changepoints_.clear();
    }
    /**
     * @brief Process a stream of data.
     *
     * @param data Vector of data points.
     */
    void process(const std::vector<double>& data) {
        for (size_t i = 1; i < data.size(); ++i) {
            updateMeanVariance(data[i]);
            updateStatistics(i, data[i]);
            decisionRule(i);
        }
    }

     /**
     * @brief Process a single new data point and determine if it's an anomaly.
     *
     * @param data_point The new data point to process.
     *
     * @return True if an anomaly is detected, False otherwise.
     */
    bool update(double data_point) {
        int i = n_;
        updateMeanVariance(data_point);
        updateStatistics(i, data_point);
        return decisionRule(i);
    }

    /**
     * @brief Get the detected changepoints.
     *
     * @return Vector of changepoint indices.
     */
    const std::vector<int>& getChangepoints() const {
        return changepoints_;
    }

    /**
     * @brief Get the current mean.
     *
     * @return The current mean.
     */
    double getMean() const {
        return mu_;
    }

    /**
    * @brief Get the current standard deviation
    *
    * @return the current standard deviation
    */
    double getSigma() const{
        return sigma_;
    }

private:
    double r_;       // Control parameter (learning rate)
    double L_;       // Control parameter (threshold)
    int burnin_;    // Burn-in period
    double mu_;       // Current mean
    double sigma_;    // Current standard deviation
    double Z_;        // Current Z statistic
    double sigma_Z_;  // Current standard deviation of Z
    std::vector<int> changepoints_; // Vector of changepoints
    int n_;           // Current run length
};

} // namespace cachelib
} // namespace facebook