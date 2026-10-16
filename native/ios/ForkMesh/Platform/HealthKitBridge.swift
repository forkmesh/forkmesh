import CoreMotion
import FMTools
import Foundation
import HealthKit

@MainActor
final class HealthKitBridge: HealthBridge {
    static let entryKey: String = "ForkMeshEntry"
    static let versionKey: String = "ForkMeshVersion"

    private static let stepTypes: [HKQuantityTypeIdentifier] = [
        .stepCount, .distanceWalkingRunning, .flightsClimbed, .activeEnergyBurned,
    ]
    private static let nutritionTypes: [HKQuantityTypeIdentifier] = [
        .dietaryEnergyConsumed, .dietaryProtein, .dietaryCarbohydrates, .dietaryFatTotal,
    ]

    private let store: HKHealthStore = HKHealthStore()
    private let pedometer: CMPedometer = CMPedometer()
    private let walkPedometer: CMPedometer = CMPedometer()
    private let pacePedometer: CMPedometer = CMPedometer()
    private let motion: CMMotionActivityManager = CMMotionActivityManager()
    nonisolated private static let motionQueue: OperationQueue = {
        let queue: OperationQueue = OperationQueue()
        queue.name = "ForkMesh.motionHistory"
        queue.qualityOfService = .utility
        queue.maxConcurrentOperationCount = 1
        return queue
    }()
    private let accelerometer: CMMotionManager = CMMotionManager()
    private var watchingMotion: Bool = false
    private var stepObserver: HKObserverQuery?
    private var sleepObserver: HKObserverQuery?

    var available: Bool { HKHealthStore.isHealthDataAvailable() }

    static func identifier(_ quantity: HealthQuantity) -> HKQuantityTypeIdentifier {
        switch quantity {
        case .water: return .dietaryWater
        case .energy: return .dietaryEnergyConsumed
        case .protein: return .dietaryProtein
        case .carbs: return .dietaryCarbohydrates
        case .fat: return .dietaryFatTotal
        }
    }

    static func unit(_ quantity: HealthQuantity) -> HKUnit {
        switch quantity {
        case .water: return HKUnit.literUnit(with: .milli)
        case .energy: return HKUnit.kilocalorie()
        case .protein, .carbs, .fat: return HKUnit.gram()
        }
    }

    func authorize(_ kinds: Set<HealthKind>) async -> Bool {
        guard available else { return false }
        var read: Set<HKObjectType> = []
        var share: Set<HKSampleType> = []
        if kinds.contains(.steps) {
            for id in Self.stepTypes { read.insert(HKQuantityType(id)) }
        }
        if kinds.contains(.water) {
            let water: HKQuantityType = HKQuantityType(.dietaryWater)
            read.insert(water)
            share.insert(water)
        }
        if kinds.contains(.nutrition) {
            for id in Self.nutritionTypes {
                let type: HKQuantityType = HKQuantityType(id)
                read.insert(type)
                share.insert(type)
            }
        }
        do {
            try await store.requestAuthorization(toShare: share, read: read)
        } catch {
            return false
        }
        if kinds.contains(.steps) && CMPedometer.isStepCountingAvailable()
            && CMPedometer.authorizationStatus() == .notDetermined {
            let now: Date = Date()
            _ = await motionReading(from: Calendar.current.startOfDay(for: now), to: now)
        }
        return share.allSatisfy { store.authorizationStatus(for: $0) == .sharingAuthorized }
    }

    func stepDays(_ count: Int, now: Date) async -> [HealthStepDay] {
        let calendar: Calendar = Calendar.current
        let today: Date = calendar.startOfDay(for: now)
        let span: Int = max(1, count)
        guard let start = calendar.date(byAdding: .day, value: -(span - 1), to: today) else { return [] }
        var days: [String: HealthStepDay] = [:]
        if available {
            let steps: [String: Double] = await dailySums(.stepCount, unit: .count(), start: start, end: now)
            let distance: [String: Double] = await dailySums(.distanceWalkingRunning, unit: .meter(), start: start, end: now)
            let floors: [String: Double] = await dailySums(.flightsClimbed, unit: .count(), start: start, end: now)
            let active: [String: Double] = await dailySums(.activeEnergyBurned, unit: .kilocalorie(), start: start, end: now)
            for day in Set(steps.keys).union(distance.keys) {
                days[day] = HealthStepDay(
                    day: day, steps: Int((steps[day] ?? 0).rounded()), distanceM: distance[day] ?? 0,
                    floors: Int((floors[day] ?? 0).rounded()), activeKcal: active[day] ?? 0)
            }
        }
        for offset in 0..<min(span, 7) {
            guard let dayStart = calendar.date(byAdding: .day, value: -offset, to: today),
                let dayEnd = calendar.date(byAdding: .day, value: 1, to: dayStart),
                let data = await motionReading(from: dayStart, to: min(dayEnd, now))
            else { continue }
            let day: String = CaloriesText.isoDay(dayStart)
            let reading: HealthStepDay = HealthStepDay(
                day: day, steps: data.numberOfSteps.intValue, distanceM: data.distance?.doubleValue ?? 0,
                floors: data.floorsAscended?.intValue ?? 0)
            days[day] = days[day].map { $0.merged(with: reading) } ?? reading
        }
        return days.values.filter { $0.steps > 0 }.sorted { $0.day < $1.day }
    }

    func startLiveSteps(_ update: @escaping @MainActor (HealthStepDay) -> Void) -> Bool {
        guard CMPedometer.isStepCountingAvailable() else { return false }
        let start: Date = Calendar.current.startOfDay(for: Date())
        let day: String = CaloriesText.isoDay(start)
        pedometer.startUpdates(from: start) { (data: CMPedometerData?, _: Error?) in
            guard let data else { return }
            let reading: HealthStepDay = HealthStepDay(
                day: day, steps: data.numberOfSteps.intValue, distanceM: data.distance?.doubleValue ?? 0,
                floors: data.floorsAscended?.intValue ?? 0, cadence: data.currentCadence?.doubleValue ?? 0,
                pace: data.currentPace?.doubleValue ?? 0)
            Task { @MainActor in update(reading) }
        }
        return true
    }

    func stopLiveSteps() {
        pedometer.stopUpdates()
    }

    func startMotionSamples(_ update: @escaping @MainActor ([MotionSample]) -> Void) -> Bool {
        guard accelerometer.isAccelerometerAvailable else { return false }
        accelerometer.stopAccelerometerUpdates()
        accelerometer.accelerometerUpdateInterval = StrideMeter.sampleInterval
        accelerometer.startAccelerometerUpdates(to: .main) { (data: CMAccelerometerData?, _: Error?) in
            guard let data else { return }
            let sample: MotionSample = MotionSample(
                x: data.acceleration.x, y: data.acceleration.y, z: data.acceleration.z, time: data.timestamp)
            MainActor.assumeIsolated { update([sample]) }
        }
        return true
    }

    func stopMotionSamples() {
        accelerometer.stopAccelerometerUpdates()
    }

    func startWalkDetection(_ update: @escaping @MainActor (WalkMotionSample) -> Void) -> Bool {
        guard CMMotionActivityManager.isActivityAvailable(), CMPedometer.isStepCountingAvailable() else { return false }
        let status: CMAuthorizationStatus = CMMotionActivityManager.authorizationStatus()
        guard status != .denied && status != .restricted else { return false }
        if watchingMotion { motion.stopActivityUpdates() }
        watchingMotion = true
        motion.startActivityUpdates(to: .main) { (activity: CMMotionActivity?) in
            guard let activity else { return }
            let sample: WalkMotionSample = Self.walkSample(activity)
            Task { @MainActor in update(sample) }
        }
        return true
    }

    func stopWalkDetection() {
        guard watchingMotion else { return }
        watchingMotion = false
        motion.stopActivityUpdates()
    }

    nonisolated static func walkSample(_ activity: CMMotionActivity) -> WalkMotionSample {
        let kind: WalkMotion
        if activity.running {
            kind = .running
        } else if activity.walking {
            kind = .walking
        } else if activity.stationary || activity.automotive || activity.cycling {
            kind = .still
        } else {
            kind = .unknown
        }
        return WalkMotionSample(motion: kind, confident: activity.confidence != .low, date: activity.startDate)
    }

    func startWalkSteps(from start: Date, _ update: @escaping @MainActor (Int, Double) -> Void) -> Bool {
        guard CMPedometer.isStepCountingAvailable() else { return false }
        walkPedometer.stopUpdates()
        walkPedometer.startUpdates(from: start) { (data: CMPedometerData?, _: Error?) in
            guard let data else { return }
            let steps: Int = data.numberOfSteps.intValue
            let distance: Double = data.distance?.doubleValue ?? 0
            Task { @MainActor in update(steps, distance) }
        }
        return true
    }

    func stopWalkSteps() {
        walkPedometer.stopUpdates()
    }

    func startPaceSteps(_ update: @escaping @MainActor (Int, Double) -> Void) -> Bool {
        guard CMPedometer.isStepCountingAvailable() else { return false }
        let status: CMAuthorizationStatus = CMPedometer.authorizationStatus()
        guard status != .denied && status != .restricted else { return false }
        pacePedometer.stopUpdates()
        pacePedometer.startUpdates(from: Date()) { (data: CMPedometerData?, _: Error?) in
            guard let data else { return }
            let steps: Int = data.numberOfSteps.intValue
            let cadence: Double = data.currentCadence?.doubleValue ?? 0
            Task { @MainActor in update(steps, cadence) }
        }
        return true
    }

    func stopPaceSteps() {
        pacePedometer.stopUpdates()
    }

    func stepsToday(until end: Date) async -> Int {
        await motionReading(from: Calendar.current.startOfDay(for: end), to: end)?.numberOfSteps.intValue ?? 0
    }

    func walkHistory(from start: Date, to end: Date) async -> [WalkMotionSample] {
        guard CMMotionActivityManager.isActivityAvailable(), end > start else { return [] }
        return await withCheckedContinuation { (continuation: CheckedContinuation<[WalkMotionSample], Never>) in
            Self.queryWalks(motion, from: start, to: end, continuation)
        }
    }

    nonisolated private static func queryWalks(
        _ motion: CMMotionActivityManager, from start: Date, to end: Date,
        _ continuation: CheckedContinuation<[WalkMotionSample], Never>
    ) {
        motion.queryActivityStarting(from: start, to: end, to: motionQueue) { (activities: [CMMotionActivity]?, _: Error?) in
            continuation.resume(returning: (activities ?? []).map(walkSample))
        }
    }

    func walkReading(from start: Date, to end: Date) async -> (steps: Int, distanceM: Double)? {
        guard let data = await motionReading(from: start, to: end) else { return nil }
        return (data.numberOfSteps.intValue, data.distance?.doubleValue ?? 0)
    }

    func samples(_ quantity: HealthQuantity, from: Date, to: Date) async throws -> [HealthSample] {
        guard available else { return [] }
        let type: HKQuantityType = HKQuantityType(Self.identifier(quantity))
        let descriptor: HKSampleQueryDescriptor<HKQuantitySample> = HKSampleQueryDescriptor(
            predicates: [.quantitySample(type: type, predicate: HKQuery.predicateForSamples(withStart: from, end: to))],
            sortDescriptors: [SortDescriptor(\.startDate)], limit: 2000)
        let results: [HKQuantitySample] = try await descriptor.result(for: store)
        let ownBundle: String = HKSource.default().bundleIdentifier
        let unit: HKUnit = Self.unit(quantity)
        return results.map { (sample: HKQuantitySample) -> HealthSample in
            let ours: Bool = sample.sourceRevision.source.bundleIdentifier == ownBundle
            return HealthSample(
                entryId: ours ? (sample.metadata?[Self.entryKey] as? String ?? "") : sample.uuid.uuidString,
                version: ours ? (sample.metadata?[Self.versionKey] as? String ?? "") : "",
                amount: sample.quantity.doubleValue(for: unit), date: sample.startDate, ours: ours)
        }
    }

    func save(_ writes: [HealthWrite]) async throws {
        var objects: [HKObject] = []
        for write in writes {
            for (quantity, amount) in write.values where amount > 0 {
                var metadata: [String: Any] = [Self.entryKey: write.entryId, Self.versionKey: write.version]
                if quantity != .water && !write.name.isEmpty {
                    metadata[HKMetadataKeyFoodType] = write.name
                }
                objects.append(HKQuantitySample(
                    type: HKQuantityType(Self.identifier(quantity)),
                    quantity: HKQuantity(unit: Self.unit(quantity), doubleValue: amount),
                    start: write.date, end: write.date, metadata: metadata))
            }
        }
        guard !objects.isEmpty else { return }
        try await store.save(objects)
    }

    func delete(_ quantities: [HealthQuantity], entryIds: [String], from: Date, to: Date) async throws {
        guard available, !entryIds.isEmpty else { return }
        let predicate: NSPredicate = NSCompoundPredicate(andPredicateWithSubpredicates: [
            HKQuery.predicateForObjects(from: HKSource.default()),
            HKQuery.predicateForObjects(withMetadataKey: Self.entryKey, allowedValues: entryIds),
        ])
        for quantity in quantities {
            _ = try await store.deleteObjects(of: HKQuantityType(Self.identifier(quantity)), predicate: predicate)
        }
    }

    func observeSteps(_ handler: @escaping @MainActor () async -> Void) {
        guard available, stepObserver == nil else { return }
        let type: HKQuantityType = HKQuantityType(.stepCount)
        let query: HKObserverQuery = HKObserverQuery(sampleType: type, predicate: nil) {
            (_: HKObserverQuery, completion: @escaping HKObserverQueryCompletionHandler, error: Error?) in
            guard error == nil else {
                completion()
                return
            }
            Task { @MainActor in
                await handler()
                completion()
            }
        }
        stepObserver = query
        store.execute(query)
        store.enableBackgroundDelivery(for: type, frequency: .hourly) { (_: Bool, _: Error?) in }
    }

    func stopObservingSteps() {
        guard let query = stepObserver else { return }
        store.stop(query)
        stepObserver = nil
        store.disableBackgroundDelivery(for: HKQuantityType(.stepCount)) { (_: Bool, _: Error?) in }
    }

    private func dailySums(_ id: HKQuantityTypeIdentifier, unit: HKUnit, start: Date, end: Date) async -> [String: Double] {
        let type: HKQuantityType = HKQuantityType(id)
        let descriptor: HKStatisticsCollectionQueryDescriptor = HKStatisticsCollectionQueryDescriptor(
            predicate: .quantitySample(type: type, predicate: HKQuery.predicateForSamples(withStart: start, end: end)),
            options: .cumulativeSum, anchorDate: start, intervalComponents: DateComponents(day: 1))
        do {
            let collection: HKStatisticsCollection = try await descriptor.result(for: store)
            var result: [String: Double] = [:]
            collection.enumerateStatistics(from: start, to: end) { (statistics: HKStatistics, _: UnsafeMutablePointer<ObjCBool>) in
                if let sum = statistics.sumQuantity() {
                    result[CaloriesText.isoDay(statistics.startDate)] = sum.doubleValue(for: unit)
                }
            }
            return result
        } catch {
            return [:]
        }
    }

    private func motionReading(from: Date, to: Date) async -> CMPedometerData? {
        guard CMPedometer.isStepCountingAvailable(), to > from else { return nil }
        return await withCheckedContinuation { (continuation: CheckedContinuation<CMPedometerData?, Never>) in
            pedometer.queryPedometerData(from: from, to: to) { (data: CMPedometerData?, _: Error?) in
                continuation.resume(returning: data)
            }
        }
    }
}

extension HealthKitBridge: SleepBridge {
    var motionAvailable: Bool { CMMotionActivityManager.isActivityAvailable() }

    nonisolated static func sleepStage(_ value: Int) -> SleepStage? {
        guard let kind = HKCategoryValueSleepAnalysis(rawValue: value) else { return nil }
        switch kind {
        case .inBed: return .inBed
        case .asleepUnspecified: return .asleep
        case .asleepCore: return .core
        case .asleepDeep: return .deep
        case .asleepREM: return .rem
        case .awake: return .awake
        @unknown default: return nil
        }
    }

    nonisolated static func restSample(_ activity: CMMotionActivity) -> SleepRestSample {
        let moving: Bool = activity.walking || activity.running || activity.automotive || activity.cycling
        return SleepRestSample(
            start: activity.startDate, still: activity.stationary && !moving, confident: activity.confidence != .low)
    }

    func authorizeSleep(write: Bool) async -> Bool {
        var granted: Bool = true
        if available {
            let type: HKCategoryType = HKCategoryType(.sleepAnalysis)
            let share: Set<HKSampleType> = write ? [type] : []
            let read: Set<HKObjectType> = [type]
            do {
                try await store.requestAuthorization(toShare: share, read: read)
            } catch {
                granted = false
            }
            if write {
                granted = granted && store.authorizationStatus(for: type) == .sharingAuthorized
            }
        }
        if motionAvailable && CMMotionActivityManager.authorizationStatus() == .notDetermined {
            let now: Date = Date()
            _ = await restHistory(from: now.addingTimeInterval(-3600), to: now)
        }
        return granted
    }

    func restHistory(from start: Date, to end: Date) async -> [SleepRestSample]? {
        guard motionAvailable, end > start else { return nil }
        let status: CMAuthorizationStatus = CMMotionActivityManager.authorizationStatus()
        guard status != .denied && status != .restricted else { return nil }
        return await withCheckedContinuation { (continuation: CheckedContinuation<[SleepRestSample]?, Never>) in
            Self.queryRest(motion, from: start, to: end, continuation)
        }
    }

    nonisolated private static func queryRest(
        _ motion: CMMotionActivityManager, from start: Date, to end: Date,
        _ continuation: CheckedContinuation<[SleepRestSample]?, Never>
    ) {
        motion.queryActivityStarting(from: start, to: end, to: motionQueue) { (activities: [CMMotionActivity]?, error: Error?) in
            guard error == nil, let activities else {
                continuation.resume(returning: nil)
                return
            }
            continuation.resume(returning: activities.map(restSample))
        }
    }

    func sleepSamples(from start: Date, to end: Date) async -> [HealthSleepSample]? {
        guard available else { return nil }
        let type: HKCategoryType = HKCategoryType(.sleepAnalysis)
        let descriptor: HKSampleQueryDescriptor<HKCategorySample> = HKSampleQueryDescriptor(
            predicates: [.categorySample(type: type, predicate: HKQuery.predicateForSamples(withStart: start, end: end))],
            sortDescriptors: [SortDescriptor(\.startDate)], limit: 5000)
        do {
            let results: [HKCategorySample] = try await descriptor.result(for: store)
            let ownBundle: String = HKSource.default().bundleIdentifier
            return results.compactMap { (sample: HKCategorySample) -> HealthSleepSample? in
                guard let stage = Self.sleepStage(sample.value) else { return nil }
                let ours: Bool = sample.sourceRevision.source.bundleIdentifier == ownBundle
                return HealthSleepSample(
                    start: sample.startDate, end: sample.endDate, stage: stage, ours: ours,
                    entryId: ours ? (sample.metadata?[Self.entryKey] as? String ?? "") : sample.uuid.uuidString,
                    version: ours ? (sample.metadata?[Self.versionKey] as? String ?? "") : "")
            }
        } catch {
            return nil
        }
    }

    func saveSleep(_ writes: [HealthSleepWrite]) async throws {
        guard available else { return }
        let type: HKCategoryType = HKCategoryType(.sleepAnalysis)
        let objects: [HKObject] = writes.filter { $0.end > $0.start }.map { (write: HealthSleepWrite) -> HKObject in
            HKCategorySample(
                type: type, value: HKCategoryValueSleepAnalysis.asleepUnspecified.rawValue, start: write.start,
                end: write.end, metadata: [Self.entryKey: write.entryId, Self.versionKey: write.version])
        }
        guard !objects.isEmpty else { return }
        try await store.save(objects)
    }

    func deleteSleep(entryIds: [String]) async throws {
        guard available, !entryIds.isEmpty else { return }
        let predicate: NSPredicate = NSCompoundPredicate(andPredicateWithSubpredicates: [
            HKQuery.predicateForObjects(from: HKSource.default()),
            HKQuery.predicateForObjects(withMetadataKey: Self.entryKey, allowedValues: entryIds),
        ])
        _ = try await store.deleteObjects(of: HKCategoryType(.sleepAnalysis), predicate: predicate)
    }

    func observeSleep(_ handler: @escaping @MainActor () async -> Void) {
        guard available, sleepObserver == nil else { return }
        let type: HKCategoryType = HKCategoryType(.sleepAnalysis)
        let query: HKObserverQuery = HKObserverQuery(sampleType: type, predicate: nil) {
            (_: HKObserverQuery, completion: @escaping HKObserverQueryCompletionHandler, error: Error?) in
            guard error == nil else {
                completion()
                return
            }
            Task { @MainActor in
                await handler()
                completion()
            }
        }
        sleepObserver = query
        store.execute(query)
        store.enableBackgroundDelivery(for: type, frequency: .immediate) { (_: Bool, _: Error?) in }
    }

    func stopObservingSleep() {
        guard let query = sleepObserver else { return }
        store.stop(query)
        sleepObserver = nil
        store.disableBackgroundDelivery(for: HKCategoryType(.sleepAnalysis)) { (_: Bool, _: Error?) in }
    }
}

extension HealthKitBridge: SugarBridge {
    func authorizeSugar() async -> Bool {
        guard available else { return false }
        let share: Set<HKSampleType> = []
        let read: Set<HKObjectType> = [HKQuantityType(.dietarySugar)]
        do {
            try await store.requestAuthorization(toShare: share, read: read)
        } catch {
            return false
        }
        return true
    }

    func sugarGrams(from start: Date, to end: Date) async -> [String: Double] {
        guard available, end > start else { return [:] }
        return await dailySums(.dietarySugar, unit: .gram(), start: start, end: end)
    }
}

extension HealthKitBridge: NutritionBridge {
    nonisolated static func nutrientType(_ nutrient: FoodNutrient) -> (id: HKQuantityTypeIdentifier, unit: HKUnit) {
        switch nutrient {
        case .energy: return (.dietaryEnergyConsumed, .kilocalorie())
        case .protein: return (.dietaryProtein, .gram())
        case .fiber: return (.dietaryFiber, .gram())
        case .iron: return (.dietaryIron, .gramUnit(with: .milli))
        case .calcium: return (.dietaryCalcium, .gramUnit(with: .milli))
        case .vitaminD: return (.dietaryVitaminD, .gramUnit(with: .micro))
        case .vitaminC: return (.dietaryVitaminC, .gramUnit(with: .milli))
        case .potassium: return (.dietaryPotassium, .gramUnit(with: .milli))
        case .magnesium: return (.dietaryMagnesium, .gramUnit(with: .milli))
        case .folate: return (.dietaryFolate, .gramUnit(with: .micro))
        case .vitaminB12: return (.dietaryVitaminB12, .gramUnit(with: .micro))
        case .sodium: return (.dietarySodium, .gramUnit(with: .milli))
        case .sugar: return (.dietarySugar, .gram())
        case .saturatedFat: return (.dietaryFatSaturated, .gram())
        }
    }

    func authorizeNutrition() async -> Bool {
        guard available else { return false }
        let share: Set<HKSampleType> = []
        let read: Set<HKObjectType> = Set(FoodNutrient.allCases.map { (nutrient: FoodNutrient) -> HKObjectType in
            HKQuantityType(Self.nutrientType(nutrient).id)
        })
        do {
            try await store.requestAuthorization(toShare: share, read: read)
        } catch {
            return false
        }
        return true
    }

    func nutrientDays(from start: Date, to end: Date) async -> [String: [FoodNutrient: Double]] {
        guard available, end > start else { return [:] }
        var days: [String: [FoodNutrient: Double]] = [:]
        for nutrient in FoodNutrient.allCases {
            let type: (id: HKQuantityTypeIdentifier, unit: HKUnit) = Self.nutrientType(nutrient)
            let sums: [String: Double] = await dailySums(type.id, unit: type.unit, start: start, end: end)
            for (day, value) in sums where value > 0 {
                days[day, default: [:]][nutrient] = value
            }
        }
        return days
    }
}

extension HealthKitBridge: StretchBridge {
    nonisolated static func activity(_ kind: StretchWorkoutKind) -> HKWorkoutActivityType {
        switch kind {
        case .flexibility: return .flexibility
        case .mindAndBody: return .mindAndBody
        }
    }

    func authorizeStretch() async -> Bool {
        guard available else { return false }
        let type: HKWorkoutType = HKObjectType.workoutType()
        do {
            try await store.requestAuthorization(toShare: [type], read: [type])
        } catch {
            return false
        }
        return store.authorizationStatus(for: type) == .sharingAuthorized
    }

    func saveStretchWorkout(_ workout: StretchWorkout) async throws {
        guard available, workout.end > workout.start else { return }
        let configuration: HKWorkoutConfiguration = HKWorkoutConfiguration()
        configuration.activityType = Self.activity(workout.kind)
        configuration.locationType = .indoor
        let builder: HKWorkoutBuilder = HKWorkoutBuilder(healthStore: store, configuration: configuration, device: .local())
        try await builder.beginCollection(at: workout.start)
        var events: [HKWorkoutEvent] = []
        for pause in workout.pauses where pause.duration > 0 && pause.start >= workout.start && pause.end <= workout.end {
            events.append(HKWorkoutEvent(type: .pause, dateInterval: DateInterval(start: pause.start, duration: 0), metadata: nil))
            events.append(HKWorkoutEvent(type: .resume, dateInterval: DateInterval(start: pause.end, duration: 0), metadata: nil))
        }
        if !events.isEmpty {
            try await builder.addWorkoutEvents(events)
        }
        let entry: String = "stretch-\(workout.routine)-\(Int64(workout.start.timeIntervalSince1970 * 1000))"
        try await builder.addMetadata([HKMetadataKeyIndoorWorkout: true, Self.entryKey: entry])
        try await builder.endCollection(at: workout.end)
        _ = try await builder.finishWorkout()
    }

    func stretchMinutes(from start: Date, to end: Date) async -> [StretchWorkoutKind: Double] {
        guard available, end > start else { return [:] }
        let kinds: NSPredicate = NSCompoundPredicate(orPredicateWithSubpredicates: StretchWorkoutKind.allCases.map {
            (kind: StretchWorkoutKind) -> NSPredicate in HKQuery.predicateForWorkouts(with: Self.activity(kind))
        })
        let range: NSPredicate = HKQuery.predicateForSamples(withStart: start, end: end)
        let predicate: NSPredicate = NSCompoundPredicate(andPredicateWithSubpredicates: [range, kinds])
        let descriptor: HKSampleQueryDescriptor<HKWorkout> = HKSampleQueryDescriptor(
            predicates: [.workout(predicate)], sortDescriptors: [])
        do {
            let workouts: [HKWorkout] = try await descriptor.result(for: store)
            var minutes: [StretchWorkoutKind: Double] = [:]
            for workout in workouts {
                let kind: StretchWorkoutKind = workout.workoutActivityType == .mindAndBody ? .mindAndBody : .flexibility
                minutes[kind, default: 0] += workout.duration / 60
            }
            return minutes
        } catch {
            return [:]
        }
    }
}

extension HealthKitBridge: HeartRateBridge {
    static let heartRateUnit: HKUnit = HKUnit.count().unitDivided(by: HKUnit.minute())

    func authorizeHeartRate() async -> Bool {
        guard available else { return false }
        let type: HKQuantityType = HKQuantityType(.heartRate)
        do {
            try await store.requestAuthorization(toShare: [type], read: [])
        } catch {
            return false
        }
        return store.authorizationStatus(for: type) == .sharingAuthorized
    }

    func saveHeartRate(beatsPerMinute: Double, start: Date, end: Date) async throws {
        guard available, beatsPerMinute > 0 else { return }
        let sample: HKQuantitySample = HKQuantitySample(
            type: HKQuantityType(.heartRate),
            quantity: HKQuantity(unit: Self.heartRateUnit, doubleValue: beatsPerMinute),
            start: start, end: max(start, end), metadata: [HKMetadataKeyWasUserEntered: true])
        try await store.save(sample)
    }
}
